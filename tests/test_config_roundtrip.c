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
 * config_write_toml() <-> config_parse_file() round-trip guard.
 *
 * WHY THIS EXISTS: the WebUI settings save rewrites the WHOLE dawn.toml from
 * the in-memory config (webui_config.c -> config_write_toml).  Any section the
 * parser reads but the writer does not emit is therefore SILENTLY DELETED from
 * the user's dawn.toml on the next settings save, reverting those settings to
 * defaults.  Adding a config section touches several files, and the writer is
 * the easy one to forget precisely because nothing fails loudly when you do.
 *
 * That bug has now shipped three times: the messaging bot tokens (secrets
 * writer, fixed in messaging Phase 6.5), then [jobs] and [scheduler] (both
 * found by the audit that added this test).  These cases pin the round-trip so
 * a fourth is caught in CI instead of by a user losing their settings.
 */

#include <stdio.h>
#include <string.h>

#include "config/config_env.h"
#include "config/config_parser.h"
#include "config/dawn_config.h"
#include "test_config_roundtrip_stub.h"
#include "unity.h"

#define RT_PATH "test_config_roundtrip.tmp.toml"

/* sizeof(dawn_config_t) is ~47 KB, so two is ~92 KB — survivable on the
 * main stack, but static keeps this independent of the test runner's limits. */
static dawn_config_t g_written;
static dawn_config_t g_read;

void setUp(void) {
   config_set_defaults(&g_written);
   config_set_defaults(&g_read);
}

void tearDown(void) {
   remove(RT_PATH);
}

/* Write g_written, parse it back into g_read.
 *
 * Also asserts the re-parse logged nothing at WARNING+. That is a free
 * FIELD-level guard the section-header check can't provide: config_parse_file()
 * warns on any key in the file that the section's known_keys[] doesn't list, so
 * a writer emitting a key the parser never reads fails here — for every section,
 * present and future, with no list to maintain. */
static void round_trip(void) {
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, config_write_toml(&g_written, RT_PATH),
                                 "config_write_toml failed");
   test_stub_reset_warnings();
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, config_parse_file(RT_PATH, &g_read),
                                 "config_parse_file failed");
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, test_stub_warning_count(),
                                 "re-parsing the written config logged a warning — the writer "
                                 "emitted a key or section the parser does not accept");
}

/* --- [jobs] ---------------------------------------------------------------- */

static void test_jobs_roundtrip(void) {
   /* Every value stays inside config_clamp_jobs()'s bounds so a mismatch means
    * "the writer dropped it", never "the clamp rewrote it". */
   g_written.jobs.enabled = false;
   g_written.jobs.max_concurrent_local = 3;
   g_written.jobs.max_concurrent_cloud = 9;
   g_written.jobs.max_active_jobs = 7;
   g_written.jobs.max_jobs_per_user = 5;
   g_written.jobs.max_queued_per_user = 11;
   g_written.jobs.monitor_followups_per_tick = 6;
   g_written.jobs.max_spawn_depth = 2;
   g_written.jobs.max_children_per_tree = 6;
   g_written.jobs.max_reinvokes_per_tree = 12;
   g_written.jobs.max_concurrent_reinvokes = 5;
   g_written.jobs.max_runtime_sec = 999;
   g_written.jobs.event_chunk_cap = 4242;

   round_trip();

   TEST_ASSERT_FALSE(g_read.jobs.enabled);
   TEST_ASSERT_EQUAL_INT(3, g_read.jobs.max_concurrent_local);
   TEST_ASSERT_EQUAL_INT(9, g_read.jobs.max_concurrent_cloud);
   TEST_ASSERT_EQUAL_INT(7, g_read.jobs.max_active_jobs);
   TEST_ASSERT_EQUAL_INT(5, g_read.jobs.max_jobs_per_user);
   TEST_ASSERT_EQUAL_INT(11, g_read.jobs.max_queued_per_user);
   TEST_ASSERT_EQUAL_INT(6, g_read.jobs.monitor_followups_per_tick);
   TEST_ASSERT_EQUAL_INT(2, g_read.jobs.max_spawn_depth);
   TEST_ASSERT_EQUAL_INT(6, g_read.jobs.max_children_per_tree);
   TEST_ASSERT_EQUAL_INT(12, g_read.jobs.max_reinvokes_per_tree);
   TEST_ASSERT_EQUAL_INT(5, g_read.jobs.max_concurrent_reinvokes);
   TEST_ASSERT_EQUAL_INT(999, g_read.jobs.max_runtime_sec);
   TEST_ASSERT_EQUAL_INT(4242, g_read.jobs.event_chunk_cap);
}

/* --- [scheduler] ----------------------------------------------------------- */

static void test_scheduler_roundtrip(void) {
   g_written.scheduler.enabled = false;
   g_written.scheduler.default_snooze_minutes = 17;
   g_written.scheduler.max_snooze_count = 9;
   g_written.scheduler.max_events_per_user = 123;
   g_written.scheduler.max_events_total = 456;
   g_written.scheduler.missed_event_recovery = false;
   snprintf(g_written.scheduler.missed_task_policy, sizeof(g_written.scheduler.missed_task_policy),
            "%s", "skip");
   g_written.scheduler.missed_task_max_age_sec = 4321;
   g_written.scheduler.alarm_timeout_sec = 77;
   g_written.scheduler.alarm_volume = 33;
   g_written.scheduler.event_retention_days = 21;
   g_written.scheduler.briefing_speak_aloud_on_webui_source = true;

   round_trip();

   TEST_ASSERT_FALSE(g_read.scheduler.enabled);
   TEST_ASSERT_EQUAL_INT(17, g_read.scheduler.default_snooze_minutes);
   TEST_ASSERT_EQUAL_INT(9, g_read.scheduler.max_snooze_count);
   TEST_ASSERT_EQUAL_INT(123, g_read.scheduler.max_events_per_user);
   TEST_ASSERT_EQUAL_INT(456, g_read.scheduler.max_events_total);
   TEST_ASSERT_FALSE(g_read.scheduler.missed_event_recovery);
   TEST_ASSERT_EQUAL_STRING("skip", g_read.scheduler.missed_task_policy);
   TEST_ASSERT_EQUAL_INT(4321, g_read.scheduler.missed_task_max_age_sec);
   TEST_ASSERT_EQUAL_INT(77, g_read.scheduler.alarm_timeout_sec);
   TEST_ASSERT_EQUAL_INT(33, g_read.scheduler.alarm_volume);
   TEST_ASSERT_EQUAL_INT(21, g_read.scheduler.event_retention_days);
   TEST_ASSERT_TRUE(g_read.scheduler.briefing_speak_aloud_on_webui_source);
}

/* --- section coverage ------------------------------------------------------
 * The generic half: every section config_write_toml is responsible for must
 * appear in its output.  A new section wired into the parser but not the writer
 * fails HERE with its own name, rather than silently eating user settings.
 *
 * Tool-owned sections ([shutdown], [home_assistant], ...) are excluded — they
 * are emitted via tool_registry_write_configs(), which this test stubs out.
 * [api_keys] is excluded too: it is the legacy secrets spelling, superseded by
 * [secrets], and deliberately never written back. */

static void test_all_writer_owned_sections_present(void) {
   /* Every section config_write_toml() emits, INCLUDING sub-tables. Parent-only
    * coverage would let an entire [llm.tools] or [memory.embeddings] writer block
    * be deleted while this test stayed green — each sub-table is separately
    * parsed and is exactly as losable as [jobs] was. */
   static const char *const required[] = {
      "[general]",
      "[persona]",
      "[localization]",
      "[audio]",
      "[audio.bargein]",
      "[vad]",
      "[vad.chunking]",
      "[asr]",
      "[tts]",
      "[commands]",
      "[llm]",
      "[llm.cloud]",
      "[llm.local]",
      "[llm.thinking]",
      "[llm.tools]",
      "[llm.silent_observe]",
      "[memory]",
      "[memory.decay]",
      "[memory.embeddings]",
      "[memory.entity_merge]",
      "[memory.focus_injection]",
      "[memory.focus_injection.dedup]",
      "[memory.focus_injection.dominant_token_heuristic]",
      "[memory.focus_injection.source_weights]",
      "[memory.graph_retrieval]",
      "[memory.recovery]",
      "[webui]",
      "[network]",
      "[mqtt]",
      "[music]",
      "[music.plex]",
      "[music.streaming]",
      "[messaging.sms]",
      "[search]",
      "[search.summarizer]",
      "[documents]",
      "[images]",
      "[vision]",
      "[debug]",
      "[paths]",
      "[url_fetcher]",
      "[url_fetcher.flaresolverr]",
      "[url_fetcher.tavily]",
      "[calendar]",
      "[tui]",
      "[ota]",
      "[attention]",
      "[scheduler]",
      "[jobs]",
      "[mcp]",
      "[code_projects]",
   };

   /* Two sections are legitimately CONDITIONAL — [persona] is emitted only when
    * a description exists, [url_fetcher] only when it holds non-default content.
    * That is safe (the emit condition is derived from the parsed content, so
    * anything a user actually set round-trips); it just means the section is
    * absent at defaults.  Give both a triggering value so this test exercises
    * their real write path instead of asserting on an omission the writer is
    * correct to make. */
   snprintf(g_written.persona.description, sizeof(g_written.persona.description), "%s",
            "test persona");
   snprintf(g_written.url_fetcher.fallback, sizeof(g_written.url_fetcher.fallback), "%s", "tavily");

   TEST_ASSERT_EQUAL_INT(0, config_write_toml(&g_written, RT_PATH));

   FILE *fp = fopen(RT_PATH, "rb");
   TEST_ASSERT_NOT_NULL(fp);
   static char buf[512 * 1024];
   size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
   fclose(fp);
   buf[n] = '\0';
   TEST_ASSERT_GREATER_THAN_UINT(0, (unsigned)n);
   /* A truncated read would report every trailing section as "never emitted" —
    * the most misleading possible failure from the one test whose message is
    * engineered to be self-explanatory. Fail loudly on the real cause instead. */
   TEST_ASSERT_LESS_THAN_MESSAGE(sizeof(buf) - 1, n,
                                 "config output exceeded the read buffer; "
                                 "grow buf[] — section results are unreliable");

   /* Collect every miss before failing, so one run names all of them. */
   char missing[512] = { 0 };
   size_t used = 0;
   for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
      if (strstr(buf, required[i]) == NULL) {
         int w = snprintf(missing + used, sizeof(missing) - used, "%s%s", used ? " " : "",
                          required[i]);
         if (w > 0 && (size_t)w < sizeof(missing) - used) {
            used += (size_t)w;
         }
      }
   }
   if (missing[0] != '\0') {
      char msg[768];
      snprintf(msg, sizeof(msg),
               "config_write_toml() never emits: %s — a WebUI settings save will DELETE "
               "these from dawn.toml and revert those settings to defaults",
               missing);
      TEST_FAIL_MESSAGE(msg);
   }
}

/* A written file must re-parse cleanly — a malformed emit (bad quoting, stray
 * newline) would otherwise surface only as mysterious settings loss at runtime. */
static void test_written_file_reparses(void) {
   round_trip();
   TEST_ASSERT_EQUAL_STRING(g_written.general.ai_name, g_read.general.ai_name);
}

/* String values must be ESCAPED, never fprintf'd raw.  A raw "%s" write lets a
 * value close its own quote and forge TOML keys — an admin could then set
 * options the settings POST handler deliberately refuses to accept — and a raw
 * control character emits a dawn.toml the parser rejects on the next boot,
 * wedging the config from one bad value. */
static void test_string_values_cannot_forge_toml(void) {
   /* Closes the quote, then tries to flip an unrelated setting and open a new
    * table — the classic breakout. */
   snprintf(g_written.webui.www_path, sizeof(g_written.webui.www_path), "%s",
            "/var/www\"\nrequire_tls = true\n\n[bogus]\nx = \"y");
   g_written.ota.require_tls = false;

   round_trip();

   /* Survives as DATA, byte-identical — not as structure. */
   TEST_ASSERT_EQUAL_STRING(g_written.webui.www_path, g_read.webui.www_path);
   /* And the smuggled assignment did NOT take effect. */
   TEST_ASSERT_FALSE(g_read.ota.require_tls);
}

static void test_control_characters_survive_the_round_trip(void) {
   /* Raw C0 controls are illegal in a TOML basic string; they must go out as
    * \uXXXX or the next boot cannot parse the file at all. */
   snprintf(g_written.general.room, sizeof(g_written.general.room), "%s", "lab\x01\x1f\x7f end");

   round_trip(); /* asserts a clean re-parse with zero warnings */

   TEST_ASSERT_EQUAL_STRING(g_written.general.room, g_read.general.room);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_jobs_roundtrip);
   RUN_TEST(test_scheduler_roundtrip);
   RUN_TEST(test_all_writer_owned_sections_present);
   RUN_TEST(test_written_file_reparses);
   RUN_TEST(test_string_values_cannot_forge_toml);
   RUN_TEST(test_control_characters_survive_the_round_trip);
   return UNITY_END();
}
