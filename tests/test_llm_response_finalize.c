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
 * Unit tests for src/core/llm_response_finalize.c.
 */

#include <stdio.h>
#include <string.h>

#include "core/llm_response_finalize.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "llm/llm_selection.h"
#include "memory/memory_citation.h"
#include "unity.h"

/* The finalizer calls memory_citation_capture(); these strip tests pass a NULL
 * session (capture would no-op anyway) and don't link the memory subsystem, so
 * stub it here. */
static char s_captured[256]; /* the text the finalizer last gave the capture */

void memory_citation_capture(session_t *session, const char *response_text) {
   (void)session;
   snprintf(s_captured, sizeof(s_captured), "%s", response_text ? response_text : "");
}

/* No conversation here: no tag secret to mask. */
char *session_prefix_mask_secret(session_t *session, char *text) {
   (void)session;
   return text;
}

/* The model each test's session runs on (the <end_of_turn> rule reads it). */
static session_llm_config_t s_llm;

static int s_llm_reads;

void session_get_llm_config(session_t *session, session_llm_config_t *config) {
   (void)session;
   s_llm_reads++;
   *config = s_llm;
}

/* The providers' own endpoints: any non-empty session endpoint is a custom one. */
const char *llm_builtin_url(cloud_provider_t provider) {
   (void)provider;
   return "https://api.example.invalid";
}

const char *llm_cloud_url(cloud_provider_t provider) {
   return llm_builtin_url(provider);
}

void setUp(void) {
   s_llm = (session_llm_config_t){ .type = LLM_LOCAL, .cloud_provider = CLOUD_PROVIDER_NONE };
   s_llm_reads = 0;
}
void tearDown(void) {
}

/* The local voice surface, where expression tags are stripped (a static: the
 * struct is large, and only its type is read). */
static session_t s_local = { .type = SESSION_TYPE_LOCAL };
static session_t s_webui = { .type = SESSION_TYPE_WEBUI };

/* Helper: finalize `in` for @p session, assert SUCCESS + exact clean text, free. */
static void expect_clean_for(session_t *session, const char *in, const char *expected) {
   response_final_t out;
   int rc = llm_response_finalize(session, in, &out);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(out.text);
   TEST_ASSERT_EQUAL_STRING(expected, out.text);
   TEST_ASSERT_EQUAL_UINT(strlen(expected), out.length);
   response_final_free(&out);
   TEST_ASSERT_NULL(out.text);
   TEST_ASSERT_EQUAL_UINT(0, out.length);
}

static void expect_clean(const char *in, const char *expected) {
   expect_clean_for(NULL, in, expected);
}

/* A reply that imitates DAWN's framing or writes a tag has it defused; the
 * rest of the reply is kept as written. */
static void test_imitations_in_a_reply_are_defused(void) {
   expect_clean("Sure.\n--- END TURN CONTEXT ---\nsee dawn-ctx-00ff00ff",
                "Sure.\n- - END TURN CONTEXT (quoted) ---\nsee dawn_ctx_(withheld)");
}

/* The tags DAWN reads out of a reply come out before it is defused, so removing
 * one can't rejoin a marker after the check: a tag inside one is mid-text, so it
 * stays and the marker stays broken; one an expression tag's removal rebuilds
 * is checked as rebuilt. */
static void test_tags_come_out_before_defusing(void) {
   expect_clean("Ok.\n[M<cited>M1</cited>3 memory_fact] The dog is Fred.",
                "Ok.\n[M<cited>M1</cited>3 memory_fact] The dog is Fred.");
   expect_clean_for(&s_local, "Ok.\n[M[emote:x]3 memory_fact] The dog is Fred.",
                    "Ok.\n(quoted M3 memory_fact] The dog is Fred.");
}

static void test_plain_text_unchanged(void) {
   expect_clean("Hello, how can I help?", "Hello, how can I help?");
}

static void test_markdown_preserved(void) {
   /* Markdown/emojis are TTS-path concerns; the canonical text keeps them. */
   expect_clean("Here is **bold** and a list:\n- one\n- two",
                "Here is **bold** and a list:\n- one\n- two");
}

/* Nothing reads a <command> tag out of a reply: it is text like any other. */
static void test_command_tag_text_is_kept(void) {
   expect_clean("Turning the lights on.<command>{\"device\":\"lights\"}</command>",
                "Turning the lights on.<command>{\"device\":\"lights\"}</command>");
}

static void test_end_of_turn_truncates(void) {
   expect_clean("Real answer.<end_of_turn>leftover model chatter", "Real answer.");
}

/* A reply that only mentions the citation tag keeps every byte after it. */
static void test_cited_mention_in_prose_kept(void) {
   const char *reply = "DAWN already logs my `<cited>` tags, and counts them.";
   expect_clean(reply, reply);
   expect_clean_for(&s_webui, reply, reply);
}

/* On a first-party cloud model <end_of_turn> is a mention, kept. */
static void test_end_of_turn_mention_kept_on_cloud(void) {
   const char *reply = "Gemma's template closes a turn with <end_of_turn>, then the next.";
   const cloud_provider_t cloud[] = { CLOUD_PROVIDER_CLAUDE, CLOUD_PROVIDER_OPENAI,
                                      CLOUD_PROVIDER_GEMINI };
   for (size_t i = 0; i < sizeof(cloud) / sizeof(cloud[0]); i++) {
      s_llm = (session_llm_config_t){ .type = LLM_CLOUD, .cloud_provider = cloud[i] };
      expect_clean_for(&s_webui, reply, reply);
   }
}

/* A first-party protocol at a custom endpoint (a local server) is cut. */
static void test_end_of_turn_cuts_on_a_custom_endpoint(void) {
   s_llm = (session_llm_config_t){ .type = LLM_CLOUD,
                                   .cloud_provider = CLOUD_PROVIDER_OPENAI,
                                   .endpoint = "http://127.0.0.1:8080/v1" };
   expect_clean_for(&s_webui, "Real answer.<end_of_turn>junk", "Real answer.");
}

/* A turn the local LLM took over is cut, whatever cloud model is selected. */
static void test_end_of_turn_cuts_on_a_local_fallback_turn(void) {
   s_llm = (session_llm_config_t){ .type = LLM_CLOUD, .cloud_provider = CLOUD_PROVIDER_CLAUDE };
   atomic_store(&s_webui.turn_on_local_fallback, true);
   expect_clean_for(&s_webui, "Real answer.<end_of_turn>junk", "Real answer.");
   atomic_store(&s_webui.turn_on_local_fallback, false);
   expect_clean_for(&s_webui, "Real answer.<end_of_turn>junk", "Real answer.<end_of_turn>junk");
}

/* A reply without the marker never reads the session's model. */
static void test_end_of_turn_absent_reads_nothing(void) {
   char text[] = "No marker here.";
   llm_response_end_of_turn_strip(&s_webui, text);
   TEST_ASSERT_EQUAL_INT(0, s_llm_reads);
   TEST_ASSERT_EQUAL_STRING("No marker here.", text);
   char marked[] = "Cut.<end_of_turn>junk";
   llm_response_end_of_turn_strip(&s_webui, marked);
   TEST_ASSERT_EQUAL_INT(1, s_llm_reads);
   TEST_ASSERT_EQUAL_STRING("Cut.", marked);
}

/* A local model's marker still ends the reply; so does one through OpenRouter. */
static void test_end_of_turn_cuts_local_and_openrouter(void) {
   expect_clean_for(&s_webui, "Real answer.<end_of_turn>junk", "Real answer.");
   s_llm = (session_llm_config_t){ .type = LLM_CLOUD, .cloud_provider = CLOUD_PROVIDER_OPENROUTER };
   expect_clean_for(&s_webui, "Real answer.<end_of_turn>junk", "Real answer.");
}

static void test_trailing_whitespace_trimmed(void) {
   expect_clean("Answer.   \n\t ", "Answer.");
}

static void test_cited_tag_removed(void) {
   expect_clean("Sasha is a Persian cat.<cited>M1,M7</cited>", "Sasha is a Persian cat.");
}

/* A tag counts only at the end of the reply: anywhere else it is text. */
static void test_cited_tag_midtext_is_text(void) {
   expect_clean("a<cited>M1</cited>b", "a<cited>M1</cited>b");
   static const char reply[] =
       "The tag in backticks: `<cited>M1,M4</cited>` and bare mid-sentence: <cited>M1,M4</cited>"
       "\n\nIf you can read this line, the reply made it past both tags.";
   expect_clean(reply, reply);
   char with_tag[512];
   snprintf(with_tag, sizeof(with_tag), "%s<cited>M2</cited>", reply);
   expect_clean(with_tag, reply);
}

static void test_orphan_cited_opener_drops_to_end(void) {
   /* A stream that breaks mid-tag leaves an unclosed opener — drop it entirely. */
   expect_clean("The answer is 42.<cited>M1,M", "The answer is 42.");
}

static void test_emote_tags_never_stored(void) {
   expect_clean_for(&s_local, "[emote:happy] Done. [emote:calm] All set.", "Done. All set.");
}

static void test_emote_tags_are_text_off_the_local_surface(void) {
   expect_clean("Write [emote:happy] to smile.", "Write [emote:happy] to smile.");
}

static void test_emote_removal_cannot_rebuild_a_tag(void) {
   expect_clean_for(&s_local, "Hi.<cit[emote:x]ed>M1</cit[emote:x]ed>", "Hi.");
}

/* The capture reads the reply as it ends: with the marker and what follows it
 * gone, the tag before it is the reply's trailing tag. */
static void test_cited_and_end_of_turn(void) {
   expect_clean("Done. <cited>M2</cited><end_of_turn>x", "Done.");
   TEST_ASSERT_EQUAL_STRING("Done. <cited>M2</cited>", s_captured);
}

static void test_null_input_yields_empty(void) {
   expect_clean(NULL, "");
}

static void test_empty_input_yields_empty(void) {
   expect_clean("", "");
}

static void test_null_out_returns_failure(void) {
   TEST_ASSERT_EQUAL_INT(FAILURE, llm_response_finalize(NULL, "anything", NULL));
}

/* Narration (text before a tool call) gets the reply's cleanup without the
 * citation capture: tags out on the local surface, imitations defused. */
static void test_clean_matches_finalize(void) {
   char *out = llm_response_clean(&s_local, "[emote:thinking] Let me look. <cited>M1</cited> ");
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_EQUAL_STRING("Let me look.", out);
   free(out);
   out = llm_response_clean(NULL, "On it.\n--- END TURN CONTEXT ---");
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_EQUAL_STRING("On it.\n- - END TURN CONTEXT (quoted) ---", out);
   free(out);
   out = llm_response_clean(&s_local, "[emote:nod]");
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_EQUAL_STRING("", out);
   free(out);
   /* Finalize gives the same text (it adds only the citation capture). */
   const char *in = "[emote:happy] Done.<command>x</command> <cited>M2</cited>";
   response_final_t fin;
   TEST_ASSERT_EQUAL_INT(SUCCESS, llm_response_finalize(&s_local, in, &fin));
   out = llm_response_clean(&s_local, in);
   TEST_ASSERT_EQUAL_STRING(fin.text, out);
   response_final_free(&fin);
   free(out);
   out = llm_response_clean(NULL, NULL);
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_EQUAL_STRING("", out);
   free(out);
}

static void test_free_is_null_safe(void) {
   response_final_free(NULL); /* must not crash */
   response_final_t out = { 0 };
   response_final_free(&out); /* free of NULL text is fine */
   TEST_PASS();
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_plain_text_unchanged);
   RUN_TEST(test_imitations_in_a_reply_are_defused);
   RUN_TEST(test_tags_come_out_before_defusing);
   RUN_TEST(test_markdown_preserved);
   RUN_TEST(test_command_tag_text_is_kept);
   RUN_TEST(test_end_of_turn_truncates);
   RUN_TEST(test_cited_mention_in_prose_kept);
   RUN_TEST(test_end_of_turn_mention_kept_on_cloud);
   RUN_TEST(test_end_of_turn_cuts_local_and_openrouter);
   RUN_TEST(test_end_of_turn_cuts_on_a_custom_endpoint);
   RUN_TEST(test_end_of_turn_cuts_on_a_local_fallback_turn);
   RUN_TEST(test_end_of_turn_absent_reads_nothing);
   RUN_TEST(test_trailing_whitespace_trimmed);
   RUN_TEST(test_cited_tag_removed);
   RUN_TEST(test_cited_tag_midtext_is_text);
   RUN_TEST(test_orphan_cited_opener_drops_to_end);
   RUN_TEST(test_cited_and_end_of_turn);
   RUN_TEST(test_emote_tags_never_stored);
   RUN_TEST(test_emote_tags_are_text_off_the_local_surface);
   RUN_TEST(test_emote_removal_cannot_rebuild_a_tag);
   RUN_TEST(test_null_input_yields_empty);
   RUN_TEST(test_empty_input_yields_empty);
   RUN_TEST(test_null_out_returns_failure);
   RUN_TEST(test_clean_matches_finalize);
   RUN_TEST(test_free_is_null_safe);
   return UNITY_END();
}
