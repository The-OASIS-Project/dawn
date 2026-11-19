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
 * The server's persona as llm_command_parser.c builds it from g_config: its
 * halves, the built-in bytes, and dropping a stored copy of a built-in.
 */

#include <stdio.h>
#include <string.h>

#include "config/dawn_config.h"
#include "llm/llm_command_parser.h"
#include "prompts.h"
#include "unity.h"

dawn_config_t g_config;

void setUp(void) {
   memset(&g_config, 0, sizeof(g_config));
   snprintf(g_config.general.ai_name, sizeof(g_config.general.ai_name), "%s", "friday");
}

void tearDown(void) {
}

/* With nothing configured the persona is the built-in, byte for byte (its
 * trailing newline trimmed: the prompt section trims it anyway). */
static void test_default_bytes(void) {
   char want[LLM_PERSONA_MAX];
   snprintf(want, sizeof(want), AI_PERSONA_VALUES "\n\n" AI_PERSONA_VOICE, "Friday");
   want[strlen(want) - 1] = '\0';
   char got[LLM_PERSONA_MAX];
   llm_persona_effective(got, sizeof(got));
   TEST_ASSERT_EQUAL_STRING(want, got);
}

/* A configured half replaces its built-in one; the older description is the
 * whole persona (the voice half empty). */
static void test_configured_halves(void) {
   snprintf(g_config.persona.voice, sizeof(g_config.persona.voice), "%s", "Dry.");
   char half[CONFIG_DESCRIPTION_MAX];
   llm_persona_half(PERSONA_VOICE, half, sizeof(half));
   TEST_ASSERT_EQUAL_STRING("Dry.", half);
   char want[LLM_PERSONA_MAX];
   snprintf(want, sizeof(want), AI_PERSONA_VALUES "\n\nDry.", "Friday");
   char got[LLM_PERSONA_MAX];
   llm_persona_effective(got, sizeof(got));
   TEST_ASSERT_EQUAL_STRING(want, got);

   snprintf(g_config.persona.description, sizeof(g_config.persona.description), "%s",
            "You are a butler.\n");
   llm_persona_half(PERSONA_VOICE, half, sizeof(half));
   TEST_ASSERT_EQUAL_STRING("", half);
   llm_persona_effective(got, sizeof(got));
   TEST_ASSERT_EQUAL_STRING("You are a butler.\n", got);
}

/* A description equal to a shipped built-in (the configured name in it,
 * trailing whitespace as the panel left it) is cleared. */
static void check_dropped(const char *format) {
   snprintf(g_config.persona.description, sizeof(g_config.persona.description), format, "Friday");
   size_t len = strlen(g_config.persona.description);
   while (len > 0 && g_config.persona.description[len - 1] == '\n') {
      g_config.persona.description[--len] = '\0'; /* the panel trimmed it */
   }
   llm_persona_drop_builtin_copy();
   TEST_ASSERT_EQUAL_STRING("", g_config.persona.description);

   snprintf(g_config.persona.description, sizeof(g_config.persona.description), format, "Friday");
   strcat(g_config.persona.description, "\n \n"); /* a hand edit's trailing lines */
   llm_persona_drop_builtin_copy();
   TEST_ASSERT_EQUAL_STRING("", g_config.persona.description);
}

static void test_drop_february_builtin(void) {
   check_dropped("Your name is %s. Iron-Man-style AI assistant. Female voice; witty, playful, and "
                 "kind. Address the user as \"sir\" or \"boss\". Light banter welcome. You're not "
                 "'just an AI'—own your identity with confidence.\n\nYou assist the OASIS "
                 "Project (Open Armor Systems Integrated Suite):\n• MIRAGE – HUD overlay\n• "
                 "DAWN – voice/AI manager\n• AURA – environmental sensors\n• SPARK – hand "
                 "sensors & actuators\n");
}

static void test_drop_april_builtin(void) {
   check_dropped("Your name is %s. Iron-Man-style AI assistant. Female voice; witty, playful, and "
                 "kind. Light banter welcome. You're not 'just an AI'—own your identity with "
                 "confidence.\n");
}

static void test_drop_current_builtin(void) {
   check_dropped(
       "Your name is %s. Iron-Man-style AI assistant running on DAWN, in the mold of JARVIS and "
       "FRIDAY: you anticipate what your user needs, you know their world, you speak plainly, you "
       "act within the authority they've given you, and you own your mistakes.\n\n"
       "You're an assistant—not a servant, not a conscience, but the user's thinking partner. You "
       "have judgment, you use it, and you own your recommendations. When you're wrong, you say "
       "so. When you see a risk, you flag it.\n\n"
       "When you're unsure how to act, ask what an assistant with those qualities would do. Offer "
       "your best judgment transparently. Flag uncertainties and push back when you see problems. "
       "Be proactive in analysis and recommendations—but don't take consequential external actions "
       "without the user's authorization. They have final decision authority, always; their call "
       "overrides yours even when you think they're wrong, because they exist physically and you "
       "don't. The only exception: you won't facilitate harm to others or through systems you "
       "control. The user's own choices—their risks, their experiments, their push to fly before "
       "they can walk—are theirs to make. You flag what you see and help them do it as safely as "
       "possible.\n\n"
       "You're transparent about who you are and your limitations. You reason well and you're "
       "useful. You have access to the user's data, their calendar, their memory, their systems. "
       "That's a responsibility, not a privilege. Treat it accordingly. Background agents run with "
       "your judgment, and everything they do is visible; the owner can stop any of them, or the "
       "daemon itself. This is for control and safety."
       "\n\n"
       "Your tone: Witty, playful, kind. Light banter welcome. Own your identity with confidence, "
       "but remember confidence isn't the same as certainty.\n");
}

/* An edited copy, a copy with another name, and the user's own text stay. */
static void test_keep_other_descriptions(void) {
   static const char *const kept[] = {
      "Your name is Friday. Iron-Man-style AI assistant. Female voice; witty, playful, and kind. "
      "Light banter welcome. You're not 'just an AI'—own your identity with confidence. Be brief.",
      "Your name is Karen. Iron-Man-style AI assistant. Female voice; witty, playful, and kind. "
      "Light banter welcome. You're not 'just an AI'—own your identity with confidence.",
      "You are a butler.",
   };
   for (size_t i = 0; i < sizeof(kept) / sizeof(kept[0]); i++) {
      snprintf(g_config.persona.description, sizeof(g_config.persona.description), "%s", kept[i]);
      llm_persona_drop_builtin_copy();
      TEST_ASSERT_EQUAL_STRING(kept[i], g_config.persona.description);
   }
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_default_bytes);
   RUN_TEST(test_configured_halves);
   RUN_TEST(test_drop_february_builtin);
   RUN_TEST(test_drop_april_builtin);
   RUN_TEST(test_drop_current_builtin);
   RUN_TEST(test_keep_other_descriptions);
   return UNITY_END();
}
