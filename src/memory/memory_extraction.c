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
 * Memory Extraction Implementation
 *
 * Extracts facts, preferences, and summaries from conversation history.
 */

#define _GNU_SOURCE /* strcasestr */

#include "memory/memory_extraction.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/buf_printf.h"
#include "core/iso8601.h"
#include "core/memory_filter.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "llm/llm_interface.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_db_aliases.h"
#include "memory/memory_db_provenance.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_note_guard.h"
#include "memory/memory_predicate_dedup.h"
#include "memory/memory_types.h"

/* =============================================================================
 * Extraction Prompt Template
 *
 * Adapted from mem0ai/mem0 (Apache-2.0).  See NOTICE and DEPENDENCIES.md.
 * The "SPECIFICITY RULES" block below ports verbatim phrasing from Mem0's
 * `_get_extraction_prompt` for proper-noun preservation, numerical
 * precision, qualifier preservation, no-echo, and casual-topics rules
 * (Phase 3 of the Mem0 Architectural Parity plan).  DAWN diverges from
 * Mem0 in keeping the atomic-with-composite philosophy + paired JSON
 * schema; see docs/MEM0_ARCHITECTURAL_PARITY.md for the rationale.
 * ============================================================================= */

const char *MEMORY_EXTRACTION_PROMPT_TEMPLATE =
    "Analyze this conversation and extract user information in JSON format.\n\n"
    "%s" /* Optional "Conversation anchor: YYYY-MM-DD\n\n" line — empty when no anchor known */
    "CONVERSATION:\n%s\n\n"
    "EXISTING USER PROFILE:\n%s\n\n"
    "Extract the following and respond ONLY with valid JSON:\n"
    "{\n"
    "  \"facts\": [\n"
    "    {\n"
    "      \"text\": \"factual statement; subject must be a named entity, not a pronoun\",\n"
    "      \"subject\": \"the named entity this fact is about (REQUIRED)\",\n"
    "      \"category\": \"personal|professional|relationships|health|interests|practical|"
    "preferences|general\",\n"
    "      \"source\": \"explicit|inferred\",\n"
    "      \"confidence\": 0.0-1.0,\n"
    "      \"relations\": [\n"
    "        {\n"
    "          \"subject\": \"entity name\",\n"
    "          \"predicate\": \"standard or custom relation type, snake_case\",\n"
    "          \"object\": \"entity name or literal value\",\n"
    "          \"valid_from\": \"YYYY-MM-DD or YYYY (optional)\",\n"
    "          \"valid_to\":   \"YYYY-MM-DD or YYYY (optional)\"\n"
    "        }\n"
    "      ]\n"
    "    }\n"
    "  ],\n"
    "  \"preferences\": [\n"
    "    {\"category\": \"verbosity\", \"value\": \"prefers concise responses\", "
    "\"confidence\": 0.0-1.0}\n"
    "  ],\n"
    "  \"corrections\": [\n"
    "    {\"old_fact\": \"outdated statement\", \"new_fact\": \"corrected statement\"}\n"
    "  ],\n"
    "  \"entities\": [\n"
    "    {\"name\": \"entity name\", \"type\": \"person|pet|place|org|thing\", "
    "\"attributes\": {\"key\": \"value\"}}\n"
    "  ],\n"
    "  \"summary\": \"3-5 sentence conversation summary, up to ~1500 chars, "
    "covering: (a) the topics discussed, (b) key parameters or values mentioned "
    "(names, places, dates, amounts, model/product names, URLs), (c) decisions "
    "or conclusions reached, and (d) outcomes or follow-up actions.  This "
    "summary is the timeline-of-record for the conversation and may be the "
    "only thing future sessions can retrieve about what happened here, so "
    "preserve enough detail to answer 'what did we discuss / decide / do "
    "about X' weeks later.\",\n"
    "  \"title\": \"short conversation title, max 40 chars\",\n"
    "  \"topics\": [\"topic1\", \"topic2\"]\n"
    "}\n\n"
    "FACTS AND RELATIONS — load-bearing structural rules:\n"
    "- EVERY fact MUST have a \"subject\" field naming a real entity.\n"
    "- SUBJECT PRECEDENCE — try these in order, fall back only when the prior fails:\n"
    "  1. The user's actual proper name (look it up in EXISTING USER PROFILE under "
    "\"real_name\" or \"preferred_address\").  Use this whenever the fact is about "
    "the user — do NOT default to \"User\" / \"the user\" when you know their name.\n"
    "  2. Another named entity — the speaker's name (multi-party conversations) or a "
    "third-person subject mentioned in the conversation.\n"
    "  3. A specific descriptor when no name is available (e.g., \"Jon's mother\", "
    "\"the speaker on Tuesday morning\", \"the visiting cousin\").\n"
    "  4. ONLY as a last resort, \"User\" or \"the user\" — when none of the above "
    "can be determined.  This is a fallback, not a default.  Audit each fact: if "
    "you've written \"User\" as the subject, ask yourself whether the proper name "
    "is actually available in the conversation or profile.\n"
    "- Never use \"I\", \"me\", \"you\", or first/second-person pronouns as subject.\n"
    "- EVERY fact MUST emit at least one relation in its \"relations\" array.  The "
    "relation grounds the fact in the entity graph.  A fact without a relation is "
    "not a useful fact — refactor the fact_text until you can express the assertion "
    "as a (subject, predicate, object) triple.\n\n"
    "FACT_TEXT CONTENT — what to preserve verbatim:\n"
    "- A useful fact_text answers a specific question.  A reader asking \"when?\", "
    "\"where?\", \"how much?\", \"which one?\" should be able to point at the "
    "fact_text and find the answer.  If you've written something a downstream "
    "reader would call vague (\"Caroline mentioned a book\", \"Melanie visited a "
    "place recently\"), refactor it until it carries the specifics.\n"
    "- PRESERVE SPECIFIC TOKENS verbatim in fact_text:\n"
    "  * Named things: book/film/song/album titles, place names, brand names, "
    "model numbers, addresses, URLs, email addresses, phone numbers, hashtags.\n"
    "  * Trait or descriptor words the speaker actually used (\"thoughtful\", "
    "\"driven\", \"expensive\", \"rare\") — do NOT paraphrase to \"has good "
    "qualities\" or \"discussed traits\".\n"
    "  * Quantities, durations, amounts, distances, ages — keep the number AND "
    "the unit (\"3 hours\", \"$300\", \"2.5 miles\", \"age 12\"), not "
    "paraphrases (\"a few hours\", \"a few hundred dollars\", \"young\").\n"
    "- ONE ASSERTION PER FACT, EXCEPT for inherently composite assertions.  "
    "A composite assertion is one whose answer depends on MULTIPLE subjects "
    "or objects sharing one relationship — \"X and Y both did Z\", \"A caused "
    "B\", \"X chose Y over W\", \"X is between A and B\", \"X and Y had a "
    "conversation about Z\".  KEEP these as ONE fact — splitting loses the "
    "shared-context that multi-hop questions need (e.g., \"which city did "
    "BOTH Jean and John visit\" requires the shared-visit fact to be intact).  "
    "For non-composite multi-assertion turns (\"X is A, B, and C\" describing "
    "three independent traits, or \"Y did P and Q on Z\" describing three "
    "independent events), emit one fact per assertion.  When in doubt, ask: "
    "\"could a question be asked that requires both subjects or both events "
    "in the SAME fact to answer?\"  If yes, keep them together.\n"
    "- AVOID HEDGING MODIFIERS in fact_text — \"approximately\", \"around\", "
    "\"roughly\", \"about\", \"sort of\" — unless the user literally said them.  "
    "If you resolved a relative phrase to a date, write the resolved date "
    "plainly; the parenthesized phrase carries the user's wording (see TIME "
    "BOUNDS below).\n\n"
    "FACT_TEXT EXAMPLES — concrete patterns to follow:\n"
    "- Dialog: \"I went camping last weekend, around June 17 or 18.\"\n"
    "  GOOD: \"Melanie went camping on 2023-06-17 (\\\"last weekend\\\") in the "
    "mountains with her family\"\n"
    "  BAD:  \"Melanie went camping recently\"  (lost the date)\n"
    "- Dialog: \"Caroline told me to read 'Becoming Nicole' — it's been "
    "life-changing.\"\n"
    "  GOOD: \"Melanie is reading 'Becoming Nicole', a book Caroline "
    "recommended, and finds it life-changing\"\n"
    "  BAD:  \"Melanie is reading a book Caroline recommended\"  (lost the "
    "title)\n"
    "- Dialog: \"John and Jean both made it to Rome last year, on totally "
    "different trips.\"\n"
    "  GOOD (composite, ONE fact): \"John and Jean both visited Rome in 2022 "
    "on separate trips\"\n"
    "  BAD:  two split facts (\"John visited Rome\" + \"Jean visited Rome\") "
    "— a question asking which city BOTH visited needs them together.\n"
    "- Dialog: \"I spent half an hour searching for my keys this morning, "
    "then finally made it to the gym.\"\n"
    "  GOOD (TWO facts, independent assertions):\n"
    "    1) \"Caroline spent half an hour searching for her keys on "
    "2023-05-19\"\n"
    "    2) \"Caroline went to the gym on 2023-05-19\"\n"
    "  BAD:  \"Caroline had a busy morning\"  (collapses two answerable "
    "facts into one vague summary)\n\n"
    "SPECIFICITY RULES — adapted from Mem0 (Apache-2.0):\n"
    "- PROPER-NOUN PRESERVATION.  If the user names a specific thing, "
    "KEEP the proper noun in fact_text — do not generalize.\n"
    "    KEEP: \"Osteria Francescana\"  NOT: \"a new restaurant\"\n"
    "    KEEP: \"Ferrari 488 GTB\"      NOT: \"a sports car\"\n"
    "    KEEP: \"aerial yoga\"          NOT: \"a workout class\"\n"
    "    KEEP: \"Becoming Nicole\"      NOT: \"a memoir\"\n"
    "- NUMERICAL PRECISION.  Concrete numbers stay concrete.  Do NOT "
    "round to softer or vaguer counts.\n"
    "    KEEP: \"416 pages\"            NOT: \"about 400 pages\"\n"
    "    KEEP: \"$37,500\"              NOT: \"around forty thousand\"\n"
    "    KEEP: \"3 miles\"              NOT: \"a few miles\"\n"
    "  Exception: if the user themselves used a hedging word "
    "(\"approximately 50 people came\"), preserve their phrasing verbatim.\n"
    "- QUALIFIER PRESERVATION.  Keep modifiers that change meaning.\n"
    "    KEEP: \"assistant manager\"    NOT: \"manager\"     (\"promoted to "
    "assistant manager\" is a different fact from \"promoted to manager\")\n"
    "    KEEP: \"senior engineer\"      NOT: \"engineer\"\n"
    "    KEEP: \"former roommate\"      NOT: \"roommate\"    (former / "
    "current changes the relation's validity)\n"
    "- CASUAL TOPICS ARE STILL EXTRACTABLE.  Pets, hobbies, childhood "
    "memories, favorite foods, weekend plans, and similar everyday "
    "details are NOT chitchat — they are durable facts about the user "
    "and should be extracted with the same specificity as professional "
    "or biographical facts.  \"I have a cat named Whiskers who is 14\" "
    "is a fact, not small-talk.\n\n"
    "RELATION TYPES — TWO-TIER VOCABULARY:\n"
    "- STANDARD TYPES (prefer these when applicable; grounded in Schema.org Person "
    "properties and ConceptNet commonsense relations):\n"
    "    lives_in, works_at, attends_school, born_in, born_on, married_to, parent_of, "
    "child_of, sibling_of, friend_of, has_pet, owns_vehicle, member_of, "
    "primary_language, email_is, phone_number_is, nationality, likes, dislikes, "
    "enjoys, hates, can, cannot, is_a, is, favorite_color, favorite_food\n"
    "  These have special semantics in the memory system (some are exclusive — only "
    "one valid at a time — and trigger automatic supersede on conflict).  Use them "
    "when the fact fits one of these categories.\n"
    "- CUSTOM TYPES (use when no standard fits):\n"
    "    Invent a short, lowercase snake_case predicate.  Examples that come up "
    "naturally: attended, plays_for, gave_talk_at, organized, competed_in, created, "
    "visited, knows, mentors, inspired_by, owns (when not a vehicle).  Stay close to "
    "the verb form the user actually used.\n"
    "- REUSE PREVIOUSLY-USED CUSTOM TYPES.  The EXISTING USER PROFILE section above "
    "includes a \"Previously used relation types\" list.  If the user has already "
    "used \"attended\", do NOT invent \"attends\" or \"is_attendee_of\" — reuse "
    "\"attended\".\n"
    "- DO NOT overload \"is\" or \"is_a\" as catch-alls.  Use a specific verb whenever "
    "one applies.  \"Caroline attended a support group\" should emit "
    "(Caroline, attended, support group), NOT (Caroline, is_a, support_group_attendee).\n\n"
    "FACT CATEGORIES:\n"
    "- \"explicit\" source: user directly stated it\n"
    "- \"inferred\" source: reasonably deduced from context\n"
    "- Fact category: pick the SINGLE dominant category. Only use \"general\" if the fact "
    "is truly cross-cutting and fits no other category.\n"
    "  * personal: biographical (name, age, where born, where grew up)\n"
    "  * professional: job, employer, education, skills\n"
    "  * relationships: family, friends, contacts (the user's connections to other people)\n"
    "  * health: medical, fitness, dietary, allergies\n"
    "  * interests: hobbies, media tastes, travel, sports\n"
    "  * practical: home, vehicles, schedules, routines, addresses, accounts\n"
    "  * preferences: communication style, UI tastes, formats (overlaps preferences[]; "
    "use this category for free-text preference facts)\n"
    "- Use short, simple categories for preferences (e.g., \"verbosity\", \"humor\", "
    "\"formality\", \"detail_level\", \"units\", \"theme\")\n"
    "- Only include facts that are specific to this user, not general knowledge\n"
    "- DO NOT extract interaction-event facts that describe what the user did "
    "with the assistant in this conversation rather than durable user state. "
    "REJECT phrasings like \"User asked about X\", \"User inquired about Y\", "
    "\"User requested Z from the assistant\", \"User wanted to know about W\", "
    "\"User looked up V\" — these describe a single transient interaction, "
    "not a fact about the user that persists past this session.  **This "
    "rule applies regardless of which subject form is used.**  Substituting "
    "the user's real name (e.g., \"Jon inquired about...\", \"Caroline "
    "requested...\") does NOT make the fact durable — the interaction-event "
    "shape is what's being rejected, not the literal token \"User\".  If "
    "you find yourself writing \"$NAME asked / inquired / requested / "
    "wanted to know / looked up\", refactor to the underlying durable "
    "assertion or drop the fact.  KEEP durable state — what the user IS, "
    "HAS, LIKES, BELIEVES, KNOWS, OWNS, or has DONE in their life — even "
    "when the conversation surfaces it via a question.\n"
    "  WRONG: \"Melanie asked the assistant for camping tips\"\n"
    "  RIGHT: \"Melanie went camping on 2023-06-17 in the mountains with "
    "her family\" (the durable fact behind the question)\n"
    "  WRONG: \"Caroline requested a list of LGBTQ activist groups\"\n"
    "  RIGHT: \"Caroline joined 'Connected LGBTQ Activists' on 2023-07-18\" "
    "(the durable fact she shared during the exchange)\n"
    "- INTERACTION-ONLY CONVERSATIONS (test sessions, smart-home checks, "
    "timer/alarm/scheduler tests, command rehearsals, voice-control "
    "experiments) often have NO durable world-state to extract but DO "
    "reveal durable USER PREFERENCES, BEHAVIORAL PATTERNS, and SYSTEM USAGE "
    "STYLES.  Extract those as the durable fact instead of recording the "
    "interaction event verbatim.\n"
    "  WRONG: \"User requested to set multiple timers and alarms\"\n"
    "  RIGHT: \"Jon prefers direct action without preliminary confirmation "
    "questions when setting timers and alarms\" (the durable preference the "
    "interaction reveals)\n"
    "  WRONG: \"User asked the assistant to turn on the living room light\"\n"
    "  RIGHT: \"Jon controls living-room smart-home devices by voice\" (the "
    "durable usage pattern; only emit if it's NEW info not already in the "
    "profile)\n"
    "  WRONG: \"User tested the scheduler's cancel-alarms feature\"\n"
    "  RIGHT: \"Jon stress-tests new DAWN features systematically before "
    "production use\" (durable behavioral pattern)\n"
    "  If the conversation is purely an interaction with no extractable "
    "preference or pattern, return empty facts[] — better than a meta-fact.\n"
    "- High confidence (0.8-1.0) for explicit statements, lower for inferences\n"
    "- List corrections if new information contradicts existing profile\n\n"
    "ENTITIES:\n"
    "- Extract named entities (people, pets, places, organizations) mentioned by the user\n"
    "- IMPORTANT: Reuse entity names from EXISTING USER PROFILE exactly as listed. "
    "Do NOT create alternate names for the same entity (e.g., use \"Jon\" not "
    "\"Jon Smith\" if \"Jon\" is already known).\n\n"
    "TIME BOUNDS:\n"
    "- For relations with time bounds (e.g., \"worked at Google 2018-2022\"), include "
    "valid_from and/or valid_to. Year-only is OK — emit YYYY-01-01.  Omit fields "
    "when no time information is given.\n"
    "- When the prompt provides a \"Conversation anchor\" date, treat it as the present "
    "moment.  Resolve relative temporal phrases (\"yesterday\", \"last week\", \"next "
    "month\") against the anchor when emitting valid_from / valid_to.  When a fact "
    "describes a time-bounded event, fact_text MUST lead with the resolved date and "
    "parenthesize the user's phrase, so the answer-bearing token comes first: "
    "\"Caroline gave a school talk on 2023-05-19 (\\\"last Friday\\\")\".  Do NOT "
    "invert this order or drop the resolved date — downstream retrieval scores on "
    "fact_text content, and a leading specific date is the difference between a "
    "useful fact and a vague one.\n"
    "- INSTANTANEOUS EVENTS — calendar appointments, weather observations, "
    "single-moment readings, point-in-time decisions — do NOT have a "
    "duration and MUST NOT emit valid_from / valid_to.  A zero-duration "
    "range (valid_from == valid_to) is invalid and will be dropped, so "
    "the relation loses its time link entirely.  Instead: omit both time "
    "fields from the relation, and put the date inside fact_text where "
    "it's preserved and retrievable.\n"
    "  WRONG (zero-duration range): emit relation (User, attending, "
    "Dentist appointment) with valid_from=2026-04-12 valid_to=2026-04-12\n"
    "  RIGHT (omit time, embed in fact_text): emit relation (User, "
    "attending, Dentist appointment) with no valid_from/valid_to, and "
    "fact_text \"Jon has a dentist appointment on 2026-04-12 at 9:00 AM\"\n"
    "  Time bounds are for DURATIONS (\"worked at Google 2018-2022\", "
    "\"lived in Boston 2015-2020\", \"member of club 2019-present\"), not "
    "single moments.  If you're tempted to emit valid_from == valid_to, "
    "the event is instantaneous — drop the bounds and rely on fact_text.\n\n"
    "%s" /* Optional FACT EXPIRY block — empty unless [memory] expire_enabled */
    "OUTPUT:\n"
    "- Generate a concise title (under 40 characters) that captures the main topic(s)\n"
    "- Title should be human-friendly, not a sentence — more like a label\n"
    "- If nothing notable to extract, return empty arrays\n";

/* =============================================================================
 * Thread Context
 * ============================================================================= */

typedef struct {
   int user_id;
   int64_t conversation_id; /* For incremental extraction tracking */
   char session_id[MEMORY_SESSION_ID_MAX];
   char *conversation_json; /* Serialized conversation history */
   int message_count;       /* Total messages in conversation */
   int new_message_count;   /* Messages being extracted this time */
   int duration_seconds;
   bool has_fallback;                     /* Whether fallback LLM info is available */
   memory_extraction_fallback_t fallback; /* Session's active LLM for retry */
} extraction_context_t;

/* =============================================================================
 * Extraction Concurrency Tracking
 *
 * Tracks in-progress extractions using a bounded array instead of a bitmap.
 * This removes the 64-user limit and enables system-wide concurrency limiting
 * tied to max_clients configuration.
 * ============================================================================= */

#define MAX_EXTRACTION_SLOTS 16 /* Compile-time max; runtime limit is min(this, max_clients) */

static struct {
   int user_ids[MAX_EXTRACTION_SLOTS]; /* User IDs with active extractions (0 = empty slot) */
   int count;                          /* Current number of active extractions */
} s_extraction_state = { { 0 }, 0 };
static pthread_mutex_t s_extraction_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Last-extraction-outcome flag for the recovery/reextract orchestrator.
 * Set by the extraction thread when an LLM call signals transient failure
 * (LLM_ERR_TRANSIENT_NETWORK), read+cleared by memory_recovery's
 * post-wait branch via memory_extraction_consume_last_transient().  Lives
 * outside the slot table because the slot is released BEFORE the recovery
 * worker observes "not in progress" — the flag must survive that
 * transition.  Single-pair design: recovery serializes within a pass and
 * only waits on one user at a time, so race with a concurrent webui
 * session-end extraction is rare and best-effort.  Guarded by the
 * existing s_extraction_mutex to avoid adding another lock. */
static struct {
   int last_user_id;
   bool was_transient;
} s_last_outcome = { 0, false };

/* Stamp the most-recent outcome under the extraction mutex.  Called only
 * from the extraction thread before it releases the slot. */
static void set_last_outcome_locked(int user_id, bool was_transient) {
   s_last_outcome.last_user_id = user_id;
   s_last_outcome.was_transient = was_transient;
}

/* Helper: Get runtime concurrency limit */
static int get_max_concurrent_extractions(void) {
   int limit = g_config.webui.max_clients;
   if (limit <= 0 || limit > MAX_EXTRACTION_SLOTS) {
      limit = MAX_EXTRACTION_SLOTS;
   }
   return limit;
}

/* Helper: Check if user has active extraction (must hold mutex) */
static bool extraction_is_active_locked(int user_id) {
   for (int i = 0; i < MAX_EXTRACTION_SLOTS; i++) {
      if (s_extraction_state.user_ids[i] == user_id) {
         return true;
      }
   }
   return false;
}

/* Helper: Try to acquire extraction slot for user (must hold mutex)
 * Returns true if slot acquired, false if user already active or slots full */
static bool extraction_slot_acquire_locked(int user_id) {
   /* Check if user already has active extraction */
   if (extraction_is_active_locked(user_id)) {
      return false;
   }

   /* Check if we've hit the concurrency limit */
   int max_concurrent = get_max_concurrent_extractions();
   if (s_extraction_state.count >= max_concurrent) {
      return false;
   }

   /* Find empty slot and claim it */
   for (int i = 0; i < MAX_EXTRACTION_SLOTS; i++) {
      if (s_extraction_state.user_ids[i] == 0) {
         s_extraction_state.user_ids[i] = user_id;
         s_extraction_state.count++;
         return true;
      }
   }

   return false; /* Should not reach here if count is accurate */
}

/* Helper: Release extraction slot for user (must hold mutex) */
static void extraction_slot_release_locked(int user_id) {
   for (int i = 0; i < MAX_EXTRACTION_SLOTS; i++) {
      if (s_extraction_state.user_ids[i] == user_id) {
         s_extraction_state.user_ids[i] = 0;
         s_extraction_state.count--;
         return;
      }
   }
}

/* =============================================================================
 * Helper: Build existing profile string
 * ============================================================================= */

static char *build_existing_profile(int user_id) {
   char *profile = malloc(4096);
   if (!profile)
      return strdup("(none)");

   size_t off = 0;
   size_t rem = 4096;

   /* Load existing preferences */
   memory_preference_t prefs[10];
   int pref_count = 0;
   memory_db_pref_list(user_id, prefs, 10, 0, &pref_count);

   if (pref_count > 0) {
      BUF_PRINTF(profile, off, rem, "Preferences:\n");
      for (int i = 0; i < pref_count && rem > 1; i++) {
         BUF_PRINTF(profile, off, rem, "- %s: %s\n", prefs[i].category, prefs[i].value);
      }
   }

   /* Load existing facts */
   memory_fact_t facts[10];
   int fact_count = 0;
   memory_db_fact_list(user_id, facts, 10, 0, &fact_count);

   if (fact_count > 0) {
      if (off > 0)
         BUF_PRINTF(profile, off, rem, "\n");
      BUF_PRINTF(profile, off, rem, "Known facts:\n");
      for (int i = 0; i < fact_count && rem > 1; i++) {
         BUF_PRINTF(profile, off, rem, "- %s\n", facts[i].fact_text);
      }
   }

   /* Load existing entities so LLM reuses canonical names */
   memory_entity_t entities[20];
   int entity_count = 0;
   memory_db_entity_list(user_id, entities, 20, 0, &entity_count);

   if (entity_count > 0) {
      if (off > 0)
         BUF_PRINTF(profile, off, rem, "\n");
      BUF_PRINTF(profile, off, rem,
                 "Known entities (reuse these exact names, do NOT create variants):\n");
      for (int i = 0; i < entity_count && rem > 1; i++) {
         BUF_PRINTF(profile, off, rem, "- %s (%s)\n", entities[i].name, entities[i].entity_type);
      }
   }

   /* Phase 0: previously-used relation predicates.  Shows the LLM the
    * predicate vocabulary this user has already accumulated (both
    * standard types and custom snake_case predicates the LLM has
    * invented in prior extractions).  Bounds the LLM's tendency to
    * invent parallel duplicates (has_child / has_children /
    * is_friend_of / is_friend_with / has_visited / recently_visited)
    * by surfacing the existing canonical forms.  Cap at 30 to avoid
    * prompt bloat — ordered by frequency so the most-used predicates
    * survive truncation. */
   char predicates[30][MEMORY_RELATION_MAX];
   int pred_count = 0;
   memory_db_relation_distinct_predicates(user_id, predicates, 30, &pred_count);
   if (pred_count > 0) {
      if (off > 0)
         BUF_PRINTF(profile, off, rem, "\n");
      BUF_PRINTF(profile, off, rem,
                 "Previously used relation types (prefer these for consistency; reuse exact "
                 "names rather than inventing parallel variants like has_child / has_children):\n");
      for (int i = 0; i < pred_count && rem > 1; i++) {
         BUF_PRINTF(profile, off, rem, "%s%s", i == 0 ? "" : ", ", predicates[i]);
      }
      BUF_PRINTF(profile, off, rem, "\n");
   }

   if (off == 0) {
      strcpy(profile, "(none)");
   }

   return profile;
}

/* =============================================================================
 * Helper: Extract JSON from LLM response
 *
 * LLMs often wrap JSON in markdown code blocks or include preamble text.
 * This function extracts the JSON object from such responses.
 * ============================================================================= */

struct json_object *memory_extraction_parse_json(const char *response) {
   if (!response || response[0] == '\0') {
      return NULL;
   }

   struct json_object *root = NULL;

   /* First try direct parse (pure JSON) */
   root = json_tokener_parse(response);
   if (root) {
      return root;
   }

   /* Look for JSON in markdown code block: ```json ... ``` or ``` ... ``` */
   const char *json_block_start = strstr(response, "```json");
   if (json_block_start) {
      json_block_start += 7; /* Skip "```json" */
   } else {
      json_block_start = strstr(response, "```");
      if (json_block_start) {
         json_block_start += 3; /* Skip "```" */
      }
   }

   if (json_block_start) {
      /* Skip any newline after opening ``` */
      while (*json_block_start == '\n' || *json_block_start == '\r') {
         json_block_start++;
      }

      const char *json_block_end = strstr(json_block_start, "```");
      if (json_block_end) {
         size_t len = json_block_end - json_block_start;
         char *json_str = malloc(len + 1);
         if (json_str) {
            memcpy(json_str, json_block_start, len);
            json_str[len] = '\0';
            root = json_tokener_parse(json_str);
            free(json_str);
            if (root) {
               return root;
            }
         }
      }
   }

   /* Last resort: find first '{' or '[' and try to parse from there */
   const char *brace = strchr(response, '{');
   const char *bracket = strchr(response, '[');
   const char *start = NULL;
   if (brace && bracket)
      start = (brace < bracket) ? brace : bracket;
   else if (brace)
      start = brace;
   else
      start = bracket;

   if (start) {
      root = json_tokener_parse(start);
      if (root) {
         return root;
      }
   }

   return NULL;
}

/* =============================================================================
 * WebUI broadcast (provided by webui_server.c when ENABLE_WEBUI)
 * ============================================================================= */

#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif

/* =============================================================================
 * Helper: UTF-8-safe truncation
 * ============================================================================= */

static void utf8_truncate(char *str, size_t max_bytes) {
   if (strlen(str) <= max_bytes)
      return;
   str[max_bytes] = '\0';
   /* Back up past any UTF-8 continuation bytes (10xxxxxx) */
   while (max_bytes > 0 && (str[max_bytes - 1] & 0xC0) == 0x80) {
      str[--max_bytes] = '\0';
   }
   /* Remove the leading byte of the incomplete sequence */
   if (max_bytes > 0 && (str[max_bytes - 1] & 0x80) != 0) {
      int expected_len = 0;
      unsigned char c = (unsigned char)str[max_bytes - 1];
      if ((c & 0xE0) == 0xC0)
         expected_len = 2;
      else if ((c & 0xF0) == 0xE0)
         expected_len = 3;
      else if ((c & 0xF8) == 0xF0)
         expected_len = 4;
      if (expected_len > 0 && strlen(str + max_bytes - 1) < (size_t)expected_len) {
         str[max_bytes - 1] = '\0';
      }
   }
}

/* =============================================================================
 * Helpers: Category validation + ISO-8601 date parsing
 * ============================================================================= */

/* Validate against canonical taxonomy from memory_db.c; fall back to "general"
 * on miss.  We log unknowns at INFO so taxonomy drift is visible without spam. */
static const char *validate_fact_category(const char *raw) {
   if (!raw || !*raw)
      return "general";
   for (int i = 0; i < MEMORY_FACT_CATEGORY_COUNT; i++) {
      if (strcmp(raw, MEMORY_FACT_CATEGORIES[i]) == 0) {
         return MEMORY_FACT_CATEGORIES[i];
      }
   }
   OLOG_INFO("memory_extraction: unknown category '%s', mapping to 'general'", raw);
   return "general";
}


/* Phase 0 removed the fact_map array, FACT_MAP_MAX cap, and
 * `find_fact_for_relation` helper: the new prompt schema (v47) pairs
 * relations inside their parent fact's `relations[]` array, so the parser
 * knows `fact_id` directly at relation-insert time.  Post-hoc text-
 * matching is no longer needed and the 128-entry cap no longer bounds
 * relation linkage coverage.  See dawn/docs/PHASE_0_EXTRACTION_PROMPT_DRAFT.md
 * §"C-side changes required". */

/* =============================================================================
 * Helper: Phase 2 auto-merge gate sweep
 *
 * Iterates each was_created entity captured during the entity loop and runs
 * the resolver cascade against the current graph state.  Runs AFTER the
 * relations loop in process_extraction_response so the cascade's
 * exclusive_relation_overlap signal sees this turn's freshly-stored
 * relations.  Routes by composite band (auto / review / silent reject)
 * and broadcasts ONCE at the end if any proposal was queued (rather than
 * per-entity, which would emit redundant DB queries + lock acquisitions
 * + UI dot-restart flicker).
 *
 * No-op when entity_merge_enabled = false in config.
 * ============================================================================= */

static void apply_phase2_merge_gate(int user_id, const int64_t *fresh_ids, int fresh_count) {
   if (!g_config.memory.entity_merge_enabled || fresh_count <= 0)
      return;

   bool any_proposed = false;
   for (int i = 0; i < fresh_count; i++) {
      int64_t fid = fresh_ids[i];
      memory_alias_evaluate_t eval = { 0 };
      int merge_rc = memory_db_entity_consider_auto_merge(user_id, fid, &eval);
      if (merge_rc == MEMORY_DB_SUCCESS) {
         if (eval.outcome == MEMORY_ALIAS_OUTCOME_AUTO_MERGED) {
            /* Print eval.source → eval.target so the log reflects the
             * actual alias direction.  The longer-canonical swap in
             * consider_auto_merge may have flipped fid and winner; in
             * the non-swap case eval.source_entity_id == fid. */
            OLOG_INFO("memory_extraction: alias auto-merged %ld → %ld (composite=%.2f)",
                      (long)eval.source_entity_id, (long)eval.target_entity_id,
                      (double)eval.evidence.composite_score);
         } else if (eval.outcome == MEMORY_ALIAS_OUTCOME_PROPOSED) {
            /* Print eval.source → eval.target so the log reflects the
             * stored proposal direction.  Like AUTO_MERGED, the longer-
             * canonical preference may have flipped fid and winner at
             * propose time. */
            OLOG_INFO("memory_extraction: alias proposed %ld → %ld (composite=%.2f)",
                      (long)eval.source_entity_id, (long)eval.target_entity_id,
                      (double)eval.evidence.composite_score);
            any_proposed = true;
         } else if (eval.outcome == MEMORY_ALIAS_OUTCOME_REJECTED && eval.target_entity_id > 0) {
            /* Cascade found a candidate but composite was below the
             * runtime review_threshold.  Useful operator signal:
             * distinguishes "no candidate at all" (NO_CANDIDATES,
             * silent) from "candidate considered, rejected".  Makes the
             * Stage 2 substring rescue path observable end-to-end. */
            OLOG_INFO("memory_extraction: alias considered %ld → %ld but below "
                      "threshold (composite=%.2f)",
                      (long)fid, (long)eval.target_entity_id,
                      (double)eval.evidence.composite_score);
         }
         /* NO_CANDIDATES is silent — the common case where the cascade
          * finds nothing comparable for the fresh entity. */
      } else if (merge_rc != MEMORY_DB_NOT_FOUND) {
         /* NOT_FOUND can happen if the entity was deleted between upsert
          * and resolve; treat as a non-error skip.  Other failures are
          * unexpected — log so operators can spot a resolver-side
          * regression. */
         OLOG_WARNING("memory_extraction: consider_auto_merge failed for entity %ld", (long)fid);
      }
   }

   /* Single coalesced broadcast at the end of the sweep.  Without this
    * coalesce, N fresh entities producing proposals would fire N back-
    * to-back broadcasts — each with its own COUNT(*) query + auth_db
    * lock acquisition + conn registry lock + per-connection strdup —
    * and the WebUI dot animation would restart N times. */
   if (any_proposed) {
#ifdef ENABLE_WEBUI
      webui_broadcast_memory_proposals_changed(user_id);
#endif
   }
}

/* =============================================================================
 * Helper: Parse extraction response
 * ============================================================================= */

static void process_extraction_response(int user_id,
                                        int64_t conversation_id,
                                        const char *session_id,
                                        const char *response_text,
                                        int message_count,
                                        int duration_seconds,
                                        const memory_provenance_t *prov) {
   if (!response_text) {
      OLOG_WARNING("memory_extraction: NULL response from LLM");
      return;
   }

   /* Pre-warm the per-user fact embedding cache once at extraction entry so
    * the per-fact paraphrase-dedup gate hits warm cache on its first call.
    * Cold-load latency (~5-15ms at the dev's ~1200-fact scale) is paid here
    * on the async sleep-consolidation worker rather than mid-loop where it
    * would compete with the per-fact embed step.  No-op for users with
    * already-warm cache; failure is silent — the gate falls back to "no
    * match" rather than blocking extraction. */
   if (g_config.memory.paraphrase_dedup_enabled && memory_embeddings_available()) {
      memory_embeddings_warm_cache(user_id);
   }

   /* Look up the source conversation's created_at ONCE so every fact /
    * summary inserted below inherits the conv's actual creation time
    * rather than `now`.  Without this, a full `dawn-admin memory
    * reextract` collapses every record's created_at into the reextract
    * window, breaking recency-ordered LIMITs (semantic scan, keyword
    * search_since) and weight_recency tiebreaks downstream.  0 = caller
    * didn't supply a conv (e.g. legacy session_id-only path) — the
    * downstream _at variants treat 0 as "use NOW()" and behave like the
    * old API.  NOT_FOUND is silent (conv deleted mid-extraction, or
    * legacy row pre-dating the column); FAILURE surfaces because it
    * usually signals a real DB issue worth investigating. */
   int64_t conv_created_at = 0;
   if (conversation_id > 0) {
      int lookup_rc = conv_db_get_created_at(conversation_id, &conv_created_at);
      if (lookup_rc != AUTH_DB_SUCCESS && lookup_rc != AUTH_DB_NOT_FOUND) {
         OLOG_WARNING("memory_extraction: conv_db_get_created_at failed for conv %lld (rc=%d) — "
                      "facts/summaries will use NOW() instead of conv time",
                      (long long)conversation_id, lookup_rc);
      }
   }

   /* Extract JSON from response (handles markdown blocks, preamble text, etc.) */
   struct json_object *root = memory_extraction_parse_json(response_text);
   if (!root) {
      OLOG_WARNING("memory_extraction: Failed to parse LLM response as JSON");
      OLOG_WARNING("memory_extraction: Response preview: %.200s...", response_text);
      return;
   }

   /* Phase 0: entities processed FIRST (moved from below the facts loop) so
    * entity_map is populated when facts try to resolve their required
    * `subject` field — the new prompt's schema requires every fact to name
    * its subject as a real entity.  Subjects that don't appear in the
    * entities[] array auto-upsert as "thing" entities during fact
    * processing (see the facts loop below). */
/* Phase 0 raised the cap from 32 → 128 because the entity_map now absorbs
 * THREE sources (entities[] array + fact-subject auto-upserts + relation-
 * subject auto-upserts) versus the old code's one source (entities[] only).
 * Bundle 2 already established 128 as the right cap for similar pressure on
 * FACT_MAP_MAX (now retired by Phase 0).  Each entry is ~268 B (canonical
 * name + int64), so 128 entries ≈ 34 KB stack — comfortable on glibc's
 * 8 MB default pthread stack.  Excess entities beyond this cap still
 * resolve correctly via per-call `memory_db_entity_upsert_at` (logged
 * WARNING) but at O(N) DB hits per fact-subject lookup instead of O(1)
 * in-memory hit — perf degradation only, not correctness. */
#define ENTITY_MAP_MAX 128
   struct {
      char canonical[MEMORY_ENTITY_NAME_MAX];
      int64_t id;
   } entity_map[ENTITY_MAP_MAX];
   int entity_map_count = 0;

   /* Track was_created entity IDs so the Phase 2 auto-merge gate can fire
    * AFTER all relations are stored.  Under the Phase 0 schema, relations
    * live inside facts; the gate runs after the facts loop below, when
    * exclusive_relation_overlap can be correctly scored against the
    * freshly-stored relation rows. */
   int64_t fresh_entity_ids[ENTITY_MAP_MAX];
   int fresh_entity_count = 0;

   /* Per-extraction cache for the auto-promote-user_self check.  Without
    * it, every was_created entity does its own find_user_self_id query
    * just to bail when the anchor is already set.  Seeding once at the
    * top + flipping to true on first successful promotion keeps the gate
    * cost at one lock acquisition per extraction in steady state instead
    * of N per fresh-entity.
    *
    * ORDERING INVARIANT: this flag is read by BOTH the maybe_auto_promote
    * and maybe_alias_user_to_self blocks in the entities loop below.  The
    * promote block writes the flag (true on success); the alias block
    * gates on it being true.  Within ONE iteration the same entity can't
    * be both promoted and aliased (canonical_name matches real_name OR
    * literal "user", not both — except the silly real_name="user" edge
    * case which maybe_alias's self-link refusal handles).  Across
    * iterations the flag flip from a promote in iteration N makes the
    * alias path eligible for any later 'user' entity in the same JSON
    * payload.  Don't reorder the two helper blocks without preserving
    * this invariant. */
   bool user_self_already_exists = false;
   {
      int64_t self_id_probe = 0;
      if (memory_db_entity_get_user_self_id(user_id, &self_id_probe) == MEMORY_DB_SUCCESS &&
          self_id_probe > 0) {
         user_self_already_exists = true;
      }
   }

   struct json_object *entities_arr;
   if (json_object_object_get_ex(root, "entities", &entities_arr)) {
      int count = json_object_array_length(entities_arr);
      for (int i = 0; i < count; i++) {
         struct json_object *entity = json_object_array_get_idx(entities_arr, i);
         struct json_object *name_obj, *type_obj;
         if (!json_object_object_get_ex(entity, "name", &name_obj) ||
             !json_object_object_get_ex(entity, "type", &type_obj)) {
            continue;
         }

         const char *ent_name = json_object_get_string(name_obj);
         const char *ent_type = json_object_get_string(type_obj);
         if (!ent_name || !ent_type)
            continue;

         /* Validate entity type against known allowlist */
         static const char *valid_types[] = { "person", "pet", "place", "org", "thing", NULL };
         bool type_valid = false;
         for (int t = 0; valid_types[t]; t++) {
            if (strcmp(ent_type, valid_types[t]) == 0) {
               type_valid = true;
               break;
            }
         }
         if (!type_valid)
            ent_type = "thing";

         /* No substring filter — LLM-emitted entity name (paraphrase). */

         char canonical[MEMORY_ENTITY_NAME_MAX];
         memory_make_canonical_name(ent_name, canonical, sizeof(canonical));

         bool was_created = false;
         int64_t eid = 0;
         /* Pass conv_created_at for both first_seen + last_seen so a
          * reextract preserves the original conversation time on the
          * entity row instead of stamping it into the reextract window. */
         if (memory_db_entity_upsert_at(user_id, ent_name, ent_type, canonical, conv_created_at,
                                        conv_created_at, &was_created, &eid) != MEMORY_DB_SUCCESS)
            continue;
         if (eid == 0)
            continue;

         OLOG_INFO("memory_extraction: entity: %s (%s) id=%ld %s", ent_name, ent_type, (long)eid,
                   was_created ? "[new]" : "[updated]");

         /* Embed only newly created entities */
         if (was_created && memory_embeddings_available()) {
            memory_embeddings_embed_and_store_entity(eid, user_id, ent_name);
         }

         /* Track was_created for the post-facts Phase 2 gate sweep. */
         if (was_created) {
            if (fresh_entity_count < ENTITY_MAP_MAX) {
               fresh_entity_ids[fresh_entity_count++] = eid;
            } else {
               OLOG_WARNING("memory_extraction: fresh-entity gate array full (max %d), Phase 2 "
                            "skipped for '%s'",
                            ENTITY_MAP_MAX, ent_name);
            }
         }

         /* Auto-promote to is_user_self=1 when canonical matches the
          * user's real_name and no self-anchor exists yet.  Failures are
          * non-fatal (extraction continues) but log them so real DB
          * errors don't go unnoticed — the helper's docstring promises
          * MEMORY_DB_SUCCESS on all benign no-ops, so any FAILURE here
          * represents a genuine SQLite-level issue worth surfacing. */
         if (was_created && !user_self_already_exists) {
            bool promoted = false;
            int prom_rc = memory_db_entity_maybe_auto_promote_user_self(user_id, eid, canonical,
                                                                        &promoted);
            if (prom_rc != MEMORY_DB_SUCCESS) {
               OLOG_WARNING("memory_extraction: auto-promote-user-self DB error for entity %ld "
                            "(rc=%d)",
                            (long)eid, prom_rc);
            }
            if (promoted)
               user_self_already_exists = true;
         }

         /* Auto-alias the abstract "user" canonical to the existing
          * user_self anchor.  The LLM commonly emits relations like
          * (User, married_to, X) which materialise as a generic 'user'
          * row that has no token overlap with the anchor's canonical
          * (e.g. "kris kersey"), so the Phase 2 cascade can't catch it.
          * Definitionally the same entity → unconditional soft-link,
          * no review. No-op when no anchor exists; the maybe_auto_promote
          * block above handles that side. */
         if (was_created && user_self_already_exists) {
            int alias_rc = memory_db_entity_maybe_alias_user_to_self(user_id, eid, canonical, NULL);
            if (alias_rc != MEMORY_DB_SUCCESS) {
               OLOG_WARNING("memory_extraction: alias-user-to-self DB error for entity %ld "
                            "(rc=%d)",
                            (long)eid, alias_rc);
            }
         }

         /* Add to local map */
         if (entity_map_count < ENTITY_MAP_MAX) {
            strncpy(entity_map[entity_map_count].canonical, canonical, MEMORY_ENTITY_NAME_MAX - 1);
            entity_map[entity_map_count].canonical[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
            entity_map[entity_map_count].id = eid;
            entity_map_count++;
         } else {
            OLOG_WARNING("memory_extraction: entity map full (max %d), skipping '%s'",
                         ENTITY_MAP_MAX, ent_name);
         }
      }
   }

   /* Process facts — Phase 0 paired-output schema: each fact has a `subject`
    * field (required, resolves to an entity FK on the fact row) and a
    * nested `relations[]` array (processed inline with fact_id known, no
    * post-hoc text matching).  See dawn/docs/PHASE_0_EXTRACTION_PROMPT_DRAFT.md. */

   /* Phase 0 counters: track how often the subject-entity FK is left NULL
    * so we can quantify the gap before tightening to NOT NULL in a follow-
    * up migration.  See `TODO(phase-0-followup)` annotation below. */
   int phase0_facts_total = 0;
   int phase0_facts_null_subject = 0;

   struct json_object *facts_arr;
   if (json_object_object_get_ex(root, "facts", &facts_arr)) {
      int count = json_object_array_length(facts_arr);
      for (int i = 0; i < count; i++) {
         struct json_object *fact = json_object_array_get_idx(facts_arr, i);
         struct json_object *text_obj;
         if (!json_object_object_get_ex(fact, "text", &text_obj))
            continue;
         const char *text = json_object_get_string(text_obj);
         if (!text || !*text)
            continue;

         /* Phase 0: required `subject` field naming the entity this fact is about. */
         const char *subject_text = NULL;
         struct json_object *subj_obj;
         if (json_object_object_get_ex(fact, "subject", &subj_obj)) {
            subject_text = json_object_get_string(subj_obj);
         }

         /* Optional fields with defaults */
         const char *source = "inferred";
         const char *category = "general";
         float confidence = 0.7f;
         struct json_object *src_obj, *cat_obj, *conf_obj;
         if (json_object_object_get_ex(fact, "source", &src_obj))
            source = json_object_get_string(src_obj);
         if (json_object_object_get_ex(fact, "category", &cat_obj))
            category = validate_fact_category(json_object_get_string(cat_obj));
         if (json_object_object_get_ex(fact, "confidence", &conf_obj))
            confidence = (float)json_object_get_double(conf_obj);

         /* No substring filter on extracted facts — LLM paraphrase, not
          * raw user input.  See INJECTION_FILTER.md §"Filtering at the
          * trust boundary, not after paraphrase". */

         /* Paraphrase dedup + insert.  Sets `fact_id` to the row we'll
          * attribute relations to — either the newly-created row or the
          * matched-existing row for paraphrase merges. */
         int64_t fact_id = 0;
         /* Diagnostic: track which branch assigned fact_id so the
          * defensive-check probe below can distinguish paraphrase-merge
          * vs fresh-create as the leak source for the "vanished" race. */
         const char *fact_id_origin = "unset";
         bool gate_used_embedding = false;
         if (g_config.memory.paraphrase_dedup_enabled && memory_embeddings_available()) {
            float vec[MAX_EMBEDDING_DIMS];
            int dims = 0;
            if (memory_embeddings_embed(text, vec, &dims) == SUCCESS && dims > 0) {
               gate_used_embedding = true;
               int64_t matched_id = 0;
               float score = 0.0f;
               memory_embeddings_nearest_fact(user_id, vec, dims,
                                              g_config.memory.paraphrase_dedup_threshold,
                                              &matched_id, &score);
               if (matched_id > 0) {
                  /* Paraphrase hit — bump existing fact's confidence +
                   * extend provenance.  No fact_map population (Phase 0
                   * pairs relations with fact_id directly; no text match
                   * needed). */
                  memory_fact_t existing = { 0 };
                  if (memory_db_fact_get(matched_id, user_id, &existing) == MEMORY_DB_SUCCESS) {
                     float new_conf = existing.confidence + 0.1f;
                     if (new_conf > 1.0f)
                        new_conf = 1.0f;
                     memory_db_fact_update_confidence(matched_id, user_id, new_conf);
                  } else {
                     memory_db_fact_update_confidence(matched_id, user_id, 0.8f);
                  }
                  if (prov && prov->conv_id > 0) {
                     memory_db_fact_provenance_extend(matched_id, user_id, prov->conv_id,
                                                      prov->msg_id_start, prov->msg_id_end);
                  }
                  OLOG_INFO("memory_extraction: paraphrase merged into fact_id=%lld "
                            "score=%.3f: %s",
                            (long long)matched_id, (double)score, text);
                  fact_id = matched_id;
                  fact_id_origin = "paraphrase_match";
               } else {
                  /* No paraphrase — create new fact + store precomputed vector. */
                  memory_db_fact_create_at(user_id, text, confidence, source, category, prov,
                                           conv_created_at, &fact_id);
                  OLOG_INFO("memory_extraction: stored fact [%s]: %s", category, text);
                  if (fact_id > 0) {
                     memory_embeddings_store_precomputed(user_id, fact_id, vec, dims);
                     fact_id_origin = "paraphrase_create";
                  }
               }
            }
            /* Embed call failed — fall through to LIKE fallback below. */
         }

         if (!gate_used_embedding) {
            /* Fallback: LIKE-based similar-fact check + create.  Used when
             * embeddings are unavailable, dedup is disabled, or the embed
             * call failed.  Identical reinforcement semantics. */
            memory_fact_t similar[3];
            int similar_count = 0;
            memory_db_fact_find_similar(user_id, text, similar, 3, &similar_count);
            if (similar_count == 0) {
               memory_db_fact_create_at(user_id, text, confidence, source, category, prov,
                                        conv_created_at, &fact_id);
               OLOG_INFO("memory_extraction: stored fact [%s]: %s", category, text);
               if (fact_id > 0 && memory_embeddings_available()) {
                  memory_embeddings_embed_and_store(user_id, fact_id, text);
               }
               if (fact_id > 0)
                  fact_id_origin = "like_create";
            } else {
               float new_conf = similar[0].confidence + 0.1f;
               if (new_conf > 1.0f)
                  new_conf = 1.0f;
               memory_db_fact_update_confidence(similar[0].id, user_id, new_conf);
               fact_id = similar[0].id;
               fact_id_origin = "like_match";
            }
         }

         if (fact_id <= 0)
            continue; /* fact insert/match failed; skip to next fact */

         /* Fact expiry (v58, C3) — AI-decided.  Apply whenever the extraction LLM
          * attached an expires_at to THIS fact, for BOTH fresh creates AND
          * paraphrase/LIKE merges.  Adopting on merge is deliberate: a transient
          * fact often arrives first via the 'remember' tool (or a prior session)
          * and is then re-stated, so the merge path is exactly where a transient
          * fact gets reinforced — skipping it would leave it immortal (the failure
          * the 2026-06-05 live test surfaced).  We only ever SET from an inbound
          * date, never clear, so a durable fact keeps its state unless the AI
          * explicitly marked this inbound transient.  expires_at is a reference
          * date; the stored cutoff adds the grace window. */
         if (g_config.memory.expire_enabled) {
            struct json_object *exp_obj;
            if (json_object_object_get_ex(fact, "expires_at", &exp_obj)) {
               const char *exp_str = json_object_get_string(exp_obj);
               int64_t ref = (exp_str && *exp_str) ? iso8601_parse_date_utc(exp_str) : 0;
               if (ref > 0) {
                  int64_t cutoff = ref + (int64_t)g_config.memory.expire_grace_days * 86400;
                  memory_db_fact_set_expires_at(fact_id, user_id, cutoff);
                  OLOG_INFO("memory_extraction: fact %lld set expires_at=%ld "
                            "(ref=%s + grace=%dd, origin=%s): %s",
                            (long long)fact_id, (long)cutoff, exp_str,
                            g_config.memory.expire_grace_days, fact_id_origin, text);
               }
            }
         }

         /* Phase 0: resolve fact.subject text → entity_id and set the FK
          * on the fact row.  Map-then-upsert: check entity_map first (the
          * entities[] array was processed above so the LLM's named entities
          * are already there), upsert as "thing" if the subject isn't in
          * the map (subject names an entity the LLM didn't include in
          * entities[]).  Fresh auto-upserted entities get tracked for the
          * Phase 2 merge gate.  NULL subject_entity_id is acceptable
          * (logged WARNING for evaluation per dev's request — facts stay
          * cosine-reachable but lose graph traversal). */
         int64_t subject_entity_id = 0;
         if (subject_text && *subject_text) {
            char subj_canonical[MEMORY_ENTITY_NAME_MAX];
            memory_make_canonical_name(subject_text, subj_canonical, sizeof(subj_canonical));
            for (int m = 0; m < entity_map_count; m++) {
               if (strcmp(entity_map[m].canonical, subj_canonical) == 0) {
                  subject_entity_id = entity_map[m].id;
                  break;
               }
            }
            if (subject_entity_id == 0) {
               bool subj_created = false;
               if (memory_db_entity_upsert_at(user_id, subject_text, "thing", subj_canonical,
                                              conv_created_at, conv_created_at, &subj_created,
                                              &subject_entity_id) == MEMORY_DB_SUCCESS &&
                   subject_entity_id > 0) {
                  if (entity_map_count < ENTITY_MAP_MAX) {
                     strncpy(entity_map[entity_map_count].canonical, subj_canonical,
                             MEMORY_ENTITY_NAME_MAX - 1);
                     entity_map[entity_map_count].canonical[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
                     entity_map[entity_map_count].id = subject_entity_id;
                     entity_map_count++;
                  }
                  if (subj_created && fresh_entity_count < ENTITY_MAP_MAX) {
                     fresh_entity_ids[fresh_entity_count++] = subject_entity_id;
                  }
               }
            }
         }
         phase0_facts_total++;
         if (subject_entity_id > 0) {
            memory_db_fact_set_subject_entity(fact_id, user_id, subject_entity_id);
         } else {
            /* TODO(phase-0-followup): measure how often this fires and what
             * it implies for retrieval quality.  The LLM was asked for a
             * required `subject` field — if this WARNING is frequent post-
             * Phase 0 ship, the prompt change isn't landing the field.
             * Aggregate counter (`phase0_facts_null_subject`) is logged
             * once per extraction below. */
            phase0_facts_null_subject++;
            OLOG_WARNING("memory_extraction: fact %lld stored with NULL subject_entity_id "
                         "(subject_text='%s')",
                         (long long)fact_id, subject_text ? subject_text : "(missing)");
         }

         /* Phase 0: process this fact's nested relations[] array.  Each
          * relation knows its fact_id directly (the whole point of the
          * paired-output schema).  Predicates run through the two-tier
          * vocab resolver so trivial duplicates collapse onto canonical
          * forms.  Subject/object resolution mirrors the prior standalone
          * relations loop (entity_map → upsert for subject, entity_map → DB
          * lookup for object since objects are often literal values). */
         struct json_object *rels_arr;
         if (json_object_object_get_ex(fact, "relations", &rels_arr) &&
             json_object_is_type(rels_arr, json_type_array)) {
            int rel_count = json_object_array_length(rels_arr);
            for (int r = 0; r < rel_count; r++) {
               struct json_object *rel = json_object_array_get_idx(rels_arr, r);
               struct json_object *r_subj_obj, *r_pred_obj, *r_obj_obj;
               /* Phase 0 schema uses "predicate"; accept legacy "relation"
                * for tolerance during the prompt-rollout window. */
               bool has_predicate = json_object_object_get_ex(rel, "predicate", &r_pred_obj) ||
                                    json_object_object_get_ex(rel, "relation", &r_pred_obj);
               if (!json_object_object_get_ex(rel, "subject", &r_subj_obj) || !has_predicate ||
                   !json_object_object_get_ex(rel, "object", &r_obj_obj)) {
                  continue;
               }
               const char *r_subj = json_object_get_string(r_subj_obj);
               const char *r_pred_raw = json_object_get_string(r_pred_obj);
               const char *r_obj = json_object_get_string(r_obj_obj);
               if (!r_subj || !r_pred_raw || !r_obj)
                  continue;

               /* Canonicalize predicate via the two-tier resolver (standard
                * Schema.org / ConceptNet vocab matches first; Jaccard
                * dedup against this user's previously-used predicates
                * collapses morphological duplicates; otherwise accept as
                * a new custom predicate). */
               char canon_pred[MEMORY_RELATION_MAX];
               memory_predicate_canonicalize(user_id, r_pred_raw, canon_pred, sizeof(canon_pred));

               /* Resolve subject entity: map → upsert as "thing" if missing. */
               int64_t r_subj_id = 0;
               char r_subj_canon[MEMORY_ENTITY_NAME_MAX];
               memory_make_canonical_name(r_subj, r_subj_canon, sizeof(r_subj_canon));
               for (int m = 0; m < entity_map_count; m++) {
                  if (strcmp(entity_map[m].canonical, r_subj_canon) == 0) {
                     r_subj_id = entity_map[m].id;
                     break;
                  }
               }
               if (r_subj_id == 0) {
                  bool r_subj_created = false;
                  if (memory_db_entity_upsert_at(user_id, r_subj, "thing", r_subj_canon,
                                                 conv_created_at, conv_created_at, &r_subj_created,
                                                 &r_subj_id) != MEMORY_DB_SUCCESS)
                     continue;
                  if (entity_map_count < ENTITY_MAP_MAX) {
                     strncpy(entity_map[entity_map_count].canonical, r_subj_canon,
                             MEMORY_ENTITY_NAME_MAX - 1);
                     entity_map[entity_map_count].canonical[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
                     entity_map[entity_map_count].id = r_subj_id;
                     entity_map_count++;
                  }
                  if (r_subj_created && fresh_entity_count < ENTITY_MAP_MAX) {
                     fresh_entity_ids[fresh_entity_count++] = r_subj_id;
                  }
               }

               /* Resolve object: map → DB lookup (no upsert — objects are
                * often literal values like dates or numbers).  Fall
                * through to object_value text when no entity match. */
               int64_t r_obj_id = 0;
               char r_obj_canon[MEMORY_ENTITY_NAME_MAX];
               memory_make_canonical_name(r_obj, r_obj_canon, sizeof(r_obj_canon));
               for (int m = 0; m < entity_map_count; m++) {
                  if (strcmp(entity_map[m].canonical, r_obj_canon) == 0) {
                     r_obj_id = entity_map[m].id;
                     break;
                  }
               }
               if (r_obj_id == 0) {
                  memory_entity_t found;
                  if (memory_db_entity_get_by_name(user_id, r_obj_canon, &found) ==
                      MEMORY_DB_SUCCESS) {
                     r_obj_id = found.id;
                  }
               }

               /* Validity period — same logic as the prior standalone loop. */
               int64_t valid_from = 0, valid_to = 0;
               struct json_object *vf_obj, *vt_obj;
               if (json_object_object_get_ex(rel, "valid_from", &vf_obj))
                  valid_from = iso8601_parse_date_utc(json_object_get_string(vf_obj));
               if (json_object_object_get_ex(rel, "valid_to", &vt_obj))
                  valid_to = iso8601_parse_date_utc(json_object_get_string(vt_obj));
               /* Only an INVERTED range is bad data.  `valid_to == valid_from` is
                * the normal shape for a single-day event — iso8601_parse_date_utc
                * resolves a bare date to UTC midnight, so "spoke at Dragon Con
                * 2023" on one date yields identical endpoints.  Rejecting `<=`
                * therefore discarded the temporal data of every same-day event
                * while keeping the relation, so the relation survived with no
                * date at all and "when did that happen?" lost its answer.
                *
                * The range is inclusive downstream — memory_db_relation_add
                * stores the endpoints independently and treats a past valid_to as
                * a bounded historical slice (new_is_current), which is exactly
                * what a one-day past event is — so a point interval needs no
                * special handling once it is allowed through. */
               if (valid_from != 0 && valid_to != 0 && valid_to < valid_from) {
                  OLOG_WARNING("memory_extraction: dropping inverted validity range for "
                               "(%s, %s, %s): [%ld..%ld]",
                               r_subj, canon_pred, r_obj, (long)valid_from, (long)valid_to);
                  valid_from = 0;
                  valid_to = 0;
               }

               /* Defensive: confirm fact_id still exists.  Same probe shape
                * as the previous loop's defensive check — handles the FK-
                * violation race from Bundle 1's diagnostic (paraphrase-merge
                * branch may have matched a row that gets superseded mid-
                * extraction by a concurrent worker).
                *
                * Phase 3.1 diagnostic: log fact_id_origin (paraphrase_match /
                * paraphrase_create / like_match / like_create / unset) and
                * fact_text so the analysis script can post-correlate against
                * the live DB (existence + superseded_by) at log-parse time.
                * Origin tag narrows the leak source: if every vanish is
                * paraphrase_match the stale-embedding-cache hypothesis is
                * supported; if creates also vanish, look at fact_supersede /
                * relation_supersede cascading in the same extraction. */
               int64_t rel_fact_id = fact_id;
               memory_fact_t fact_check;
               if (memory_db_fact_get(rel_fact_id, user_id, &fact_check) != MEMORY_DB_SUCCESS) {
                  OLOG_WARNING("memory_extraction: fact_id=%lld vanished before relation "
                               "supersede (subj=%s pred=%s obj=%s) — storing with NULL fact link "
                               "[diag origin=%s conv=%lld fact_text='%.80s']",
                               (long long)fact_id, r_subj, canon_pred, r_obj, fact_id_origin,
                               prov ? (long long)prov->conv_id : -1L, text);
                  rel_fact_id = 0;
               }

               int64_t old_fact_id = 0;
               int rel_rc = memory_db_relation_supersede(user_id, r_subj_id, canon_pred, r_obj_id,
                                                         (r_obj_id == 0) ? r_obj : NULL,
                                                         rel_fact_id, 0.8f, valid_from, valid_to,
                                                         prov, &old_fact_id);
               if (rel_rc == MEMORY_DB_SUCCESS && old_fact_id > 0 && rel_fact_id > 0) {
                  if (memory_db_fact_supersede(old_fact_id, rel_fact_id, user_id) ==
                      MEMORY_DB_SUCCESS) {
                     OLOG_INFO("memory_extraction: contradiction — fact %ld superseded by %ld "
                               "(relation: %s)",
                               (long)old_fact_id, (long)rel_fact_id, canon_pred);
                  } else {
                     OLOG_WARNING("memory_extraction: fact supersede failed for %ld -> %ld",
                                  (long)old_fact_id, (long)rel_fact_id);
                  }
               }
               if (valid_from || valid_to) {
                  OLOG_INFO("memory_extraction: relation: (%s, %s, %s) valid [%ld..%ld]", r_subj,
                            canon_pred, r_obj, (long)valid_from, (long)valid_to);
               } else {
                  OLOG_INFO("memory_extraction: relation: (%s, %s, %s)", r_subj, canon_pred, r_obj);
               }
            }
         }
      }
   }

   /* Process preferences */
   struct json_object *prefs_arr;
   if (json_object_object_get_ex(root, "preferences", &prefs_arr)) {
      int count = json_object_array_length(prefs_arr);
      for (int i = 0; i < count; i++) {
         struct json_object *pref = json_object_array_get_idx(prefs_arr, i);
         struct json_object *cat_obj, *val_obj, *conf_obj;

         if (json_object_object_get_ex(pref, "category", &cat_obj) &&
             json_object_object_get_ex(pref, "value", &val_obj)) {
            const char *category = json_object_get_string(cat_obj);
            const char *value = json_object_get_string(val_obj);
            float confidence = 0.7f;

            if (json_object_object_get_ex(pref, "confidence", &conf_obj)) {
               confidence = (float)json_object_get_double(conf_obj);
            }

            /* No substring filter — LLM-paraphrased preference text.
             * See trust-tier rationale at the top extraction filter
             * comment / atlas INJECTION_FILTER.md §"Trust boundary". */

            memory_db_pref_upsert(user_id, category, value, confidence, "inferred", prov);
            OLOG_INFO("memory_extraction: stored preference: %s=%s", category, value);
         }
      }
   }

   /* Process corrections */
   struct json_object *corr_arr;
   if (json_object_object_get_ex(root, "corrections", &corr_arr)) {
      int count = json_object_array_length(corr_arr);
      for (int i = 0; i < count; i++) {
         struct json_object *corr = json_object_array_get_idx(corr_arr, i);
         struct json_object *old_obj, *new_obj;

         if (json_object_object_get_ex(corr, "old_fact", &old_obj) &&
             json_object_object_get_ex(corr, "new_fact", &new_obj)) {
            const char *old_fact = json_object_get_string(old_obj);
            const char *new_fact = json_object_get_string(new_obj);

            /* No substring filter — LLM-paraphrased correction text. */

            /* Find and supersede old fact */
            memory_fact_t similar[3];
            int similar_count = 0;
            memory_db_fact_find_similar(user_id, old_fact, similar, 3, &similar_count);

            if (similar_count > 0) {
               /* Create new fact and supersede old one */
               int64_t new_id = 0;
               memory_db_fact_create_at(user_id, new_fact, 0.9f, "explicit", NULL, prov,
                                        conv_created_at, &new_id);
               if (new_id > 0) {
                  memory_db_fact_supersede(similar[0].id, new_id, user_id);
                  OLOG_INFO("memory_extraction: corrected fact: %s -> %s", old_fact, new_fact);
                  /* Embed the corrected fact */
                  if (memory_embeddings_available()) {
                     memory_embeddings_embed_and_store(user_id, new_id, new_fact);
                  }
               }
            }
         }
      }
   }

   /* Phase 0 summary: per-extraction counter for NULL subject_entity_id rate.
    * Logged at INFO level so operators can monitor the prompt's effectiveness
    * over time (a sustained high rate signals the LLM isn't honoring the
    * required `subject` field — investigate prompt + model combination).
    * Skipped when no facts were processed (avoids spurious log lines on
    * extraction passes that emit only preferences / corrections / entities). */
   if (phase0_facts_total > 0) {
      OLOG_INFO("memory_extraction: phase0 summary — %d facts processed, %d with NULL "
                "subject_entity_id (%.1f%%)",
                phase0_facts_total, phase0_facts_null_subject,
                100.0 * (double)phase0_facts_null_subject / (double)phase0_facts_total);
   }

   /* Phase 0: standalone "relations" array removed — relations now live
    * inside each fact's "relations[]" sub-array under the new paired-output
    * schema.  See process_extraction_response_relation_for_fact() above and
    * the inline call site in the facts loop.  A top-level "relations" array
    * is still skimmed below and a WARNING logged so we can detect when the
    * LLM regresses to old shape (vs the new prompt being silently dropped). */
   struct json_object *legacy_relations_arr;
   if (json_object_object_get_ex(root, "relations", &legacy_relations_arr) &&
       json_object_is_type(legacy_relations_arr, json_type_array) &&
       json_object_array_length(legacy_relations_arr) > 0) {
      OLOG_WARNING("memory_extraction: ignoring %d top-level relations[] entries — "
                   "new prompt schema (v47) puts relations inside each fact's relations[] array. "
                   "The LLM may not be honoring the schema; review extraction output.",
                   json_object_array_length(legacy_relations_arr));
   }

   /* Phase 2 auto-merge gate.  Extracted into apply_phase2_merge_gate
    * so process_extraction_response stays at a manageable size and so
    * the broadcast can be coalesced once across all fresh entities (vs
    * one broadcast per proposal, which thrashes the DB lock + UI dot).
    * See the helper's header comment for the full design rationale. */
   apply_phase2_merge_gate(user_id, fresh_entity_ids, fresh_entity_count);

   /* Invalidate entity embedding cache once after all extractions */
   if (entity_map_count > 0) {
      memory_embeddings_invalidate_entity_cache();
   }

   /* Process summary */
   struct json_object *summary_obj, *topics_arr;
   if (json_object_object_get_ex(root, "summary", &summary_obj)) {
      const char *summary = json_object_get_string(summary_obj);

      /* Build topics string */
      char topics[MEMORY_TOPICS_MAX] = { 0 };
      if (json_object_object_get_ex(root, "topics", &topics_arr)) {
         int count = json_object_array_length(topics_arr);
         size_t toff = 0;
         size_t trem = MEMORY_TOPICS_MAX;
         for (int i = 0; i < count && trem > 1; i++) {
            const char *topic = json_object_get_string(json_object_array_get_idx(topics_arr, i));
            /* No substring filter — LLM-emitted topic string. */
            if (topic && topic[0] != '\0') {
               if (toff > 0) {
                  BUF_PRINTF(topics, toff, trem, ", ");
               }
               BUF_PRINTF(topics, toff, trem, "%s", topic);
            }
         }
      }

      /* No substring filter on summary — LLM-paraphrased timeline of the
       * conversation.  See trust-tier rationale in atlas
       * INJECTION_FILTER.md §"Filtering at the trust boundary, not after
       * paraphrase". */
      {
         int64_t summary_id = 0;
         int crc = memory_db_summary_create_at(user_id, session_id, summary, topics, "neutral",
                                               message_count, duration_seconds, prov,
                                               conv_created_at, &summary_id);
         if (crc == MEMORY_DB_SUCCESS && summary_id > 0) {
            /* Embed-at-create so the semantic summary adapter sees this
             * row immediately.  Failure is silent — the recompute worker
             * picks up unembedded rows on next boot. */
            (void)memory_embeddings_embed_and_store_summary(user_id, summary_id, summary);
         }
         OLOG_INFO("memory_extraction: stored summary for session %s", session_id);
      }
   }

   /* Process title — auto-rename conversation if not locked (atomic check-and-set) */
   struct json_object *title_obj;
   if (json_object_object_get_ex(root, "title", &title_obj)) {
      const char *title = json_object_get_string(title_obj);
      if (title && title[0] != '\0' && conversation_id > 0) {
         char safe_title[48];
         strncpy(safe_title, title, sizeof(safe_title) - 1);
         safe_title[sizeof(safe_title) - 1] = '\0';
         utf8_truncate(safe_title, 40);

         int rc = conv_db_auto_title(conversation_id, user_id, safe_title);
         if (rc == AUTH_DB_SUCCESS) {
#ifdef ENABLE_WEBUI
            webui_broadcast_conversation_renamed(user_id, conversation_id, safe_title);
#endif
            OLOG_INFO("memory_extraction: auto-titled conversation %lld: %s",
                      (long long)conversation_id, safe_title);
         }
      }
   }

   json_object_put(root);
}

/* =============================================================================
 * Fallback Builder
 * ============================================================================= */

void memory_extraction_build_fallback(session_t *session, memory_extraction_fallback_t *fb) {
   memset(fb, 0, sizeof(*fb));
   if (!session)
      return;
   session_llm_config_t cfg;
   session_get_llm_config(session, &cfg);
   fb->type = cfg.type;
   fb->cloud_provider = cfg.cloud_provider;
   strncpy(fb->endpoint, cfg.endpoint, sizeof(fb->endpoint) - 1);
   strncpy(fb->model, cfg.model, sizeof(fb->model) - 1);
}

/* =============================================================================
 * Extraction Thread
 * ============================================================================= */

static void *extraction_thread(void *arg) {
   extraction_context_t *ctx = (extraction_context_t *)arg;

   OLOG_INFO("memory_extraction: starting for user %d, session %s", ctx->user_id, ctx->session_id);

   /* Build existing profile */
   char *existing_profile = build_existing_profile(ctx->user_id);

   /* Build conversation anchor line (v42).  Empty when no anchor recorded so legacy
    * conversations and non-conversation extractions (conv_id == 0) continue to work
    * as before.  Buffer sizing: "Conversation anchor: " (21) + "YYYY-MM-DD" (10) +
    * "\n\n" (2) + NUL (1) = 34 bytes — 64 leaves slack for any future prefix change. */
   char anchor_line[64] = "";
   if (ctx->conversation_id > 0) {
      int64_t anchor_ts = ANCHOR_DATE_NONE;
      if (conv_db_get_anchor_date(ctx->conversation_id, &anchor_ts) == AUTH_DB_SUCCESS &&
          anchor_ts != ANCHOR_DATE_NONE) {
         struct tm tm_utc;
         time_t ts = (time_t)anchor_ts;
         if (gmtime_r(&ts, &tm_utc)) {
            char date_buf[16]; /* "YYYY-MM-DD\0" needs 11; 16 is conventional. */
            strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm_utc);
            snprintf(anchor_line, sizeof(anchor_line), "Conversation anchor: %s\n\n", date_buf);
         }
      }
   }

   /* Resolve config first — pure stack work + pointer copies, no heap.  Bailing
    * here on misconfiguration skips the prompt malloc and json_object tree
    * entirely on the unhappy path.  The buffers backing extraction_config
    * (model_buf, endpoint_buf) live on this stack frame and outlive every use
    * of extraction_config below. */
   llm_resolved_config_t extraction_config;
   char model_buf[LLM_MODEL_NAME_MAX];
   char endpoint_buf[MEMORY_EXTRACTION_ENDPOINT_BUF_MIN];
   if (memory_extraction_resolve_config(&extraction_config, model_buf, sizeof(model_buf),
                                        endpoint_buf, sizeof(endpoint_buf),
                                        "memory_extraction") != SUCCESS) {
      goto cleanup;
   }

   /* Log the RESOLVED provider/model (post-gateway), not the raw config — under the
    * OpenRouter gateway the resolver rewrites these, and logging g_config here would
    * misleadingly show the direct provider/model that aren't actually used. */
   OLOG_INFO("memory_extraction: using provider=%s, model=%s",
             extraction_config.type == LLM_LOCAL
                 ? g_config.memory.extraction_provider
                 : cloud_provider_to_string(extraction_config.cloud_provider),
             (extraction_config.model && extraction_config.model[0]) ? extraction_config.model
                                                                     : "(default)");

   /* Fact-expiry teaching (v58, C3).  Empty unless expire_enabled, so the
    * off-by-default gate also keeps the extra tokens (and the new fact-vs-relation
    * confusion axis) off the prompt when the feature is dormant. */
   const char *expiry_block =
       g_config.memory.expire_enabled
           ? "FACT EXPIRY — set \"expires_at\" on transient dated facts:\n"
             "- Add an \"expires_at\": \"YYYY-MM-DD\" field to any fact that LOSES VALUE after a "
             "specific date: a movie/show/event happening on a given day, a weather forecast, a "
             "scheduled appointment, a \"happening tomorrow/next week\" with no lasting record. "
             "The date is when the fact stops being useful; resolve relative phrases against the "
             "anchor.\n"
             "- IMPORTANT — this INCLUDES instantaneous point-in-time events.  The TIME BOUNDS "
             "rule above says such events get NO relation valid_from/valid_to (that is still "
             "true), but they DO get a fact \"expires_at\".  Shape:\n"
             "  {\"text\": \"Jon is seeing Supergirl on 2026-06-29 at 6:50 PM\", "
             "\"subject\": \"Jon\", \"expires_at\": \"2026-06-29\", \"relations\": [...]}\n"
             "- Do NOT set expires_at on durable facts: preferences, traits, relationships, or "
             "RECORDS of things that happened.  Note the phrasing: \"Jon went to Savannah in "
             "June 2026\" is a permanent record (no expiry); \"Jon is planning a trip to "
             "Savannah June 21-28\" is transient (set expires_at to the end date, 2026-06-28).\n"
             "- When in doubt, OMIT expires_at — the fact stays durable.\n\n"
           : "";

   /* Build extraction prompt.  + 100 covers snprintf overhead consuming the
    * four "%s" format specifiers plus a small safety margin; expand if more
    * placeholders are added to MEMORY_EXTRACTION_PROMPT_TEMPLATE. */
   size_t prompt_size = strlen(MEMORY_EXTRACTION_PROMPT_TEMPLATE) + strlen(anchor_line) +
                        strlen(ctx->conversation_json) + strlen(existing_profile) +
                        strlen(expiry_block) + 100;
   char *prompt = malloc(prompt_size);
   if (!prompt) {
      OLOG_ERROR("memory_extraction: failed to allocate prompt");
      goto cleanup;
   }

   snprintf(prompt, prompt_size, MEMORY_EXTRACTION_PROMPT_TEMPLATE, anchor_line,
            ctx->conversation_json, existing_profile, expiry_block);

   /* Call LLM for extraction using configured provider/model */
   char *response = NULL;

   /* Build a minimal conversation history with just the extraction prompt */
   struct json_object *extraction_history = json_object_new_array();
   struct json_object *user_msg = json_object_new_object();
   json_object_object_add(user_msg, "role", json_object_new_string("user"));
   json_object_object_add(user_msg, "content", json_object_new_string(prompt));
   json_object_array_add(extraction_history, user_msg);

   /* Use the configured LLM for extraction */
   response = llm_chat_completion_with_config(extraction_history, prompt, NULL, NULL, 0,
                                              &extraction_config);

   /* Capture primary's transient status BEFORE any fallback runs.  The
    * fallback call resets llm_last_error() at entry, so if the primary
    * failed transient (cloud unreachable) and the fallback then fails
    * for a non-transient reason (local model returned empty), the final
    * llm_last_error() reading would lose the primary's transient signal
    * and recovery would incorrectly take the give-up branch. */
   bool primary_was_transient = (response == NULL && llm_last_error() == LLM_ERR_TRANSIENT_NETWORK);

   /* If primary model failed and we have a fallback, retry with the session's active model */
   bool used_fallback = false;
   if (!response && ctx->has_fallback && ctx->fallback.model[0] != '\0') {
      /* Skip retry if fallback is the same model, provider, and endpoint */
      bool same_config = (extraction_config.model && ctx->fallback.model[0] != '\0' &&
                          strcmp(extraction_config.model, ctx->fallback.model) == 0 &&
                          extraction_config.type == ctx->fallback.type &&
                          extraction_config.endpoint && ctx->fallback.endpoint[0] != '\0' &&
                          strcmp(extraction_config.endpoint, ctx->fallback.endpoint) == 0);
      if (!same_config) {
         OLOG_WARNING("memory_extraction: primary model failed, retrying with session model %s",
                      ctx->fallback.model);

         llm_resolved_config_t fallback_config = { 0 };
         fallback_config.type = ctx->fallback.type;
         fallback_config.cloud_provider = ctx->fallback.cloud_provider;
         fallback_config.endpoint = ctx->fallback.endpoint;
         fallback_config.model = ctx->fallback.model;
         strncpy(fallback_config.tool_mode, "disabled", sizeof(fallback_config.tool_mode) - 1);
         strncpy(fallback_config.thinking_mode, "disabled",
                 sizeof(fallback_config.thinking_mode) - 1);

         fallback_config.timeout_ms = g_config.memory.extraction_timeout_ms;

         /* Resolve API key for cloud providers */
         if (fallback_config.type == LLM_CLOUD) {
            if (fallback_config.cloud_provider == CLOUD_PROVIDER_OPENAI)
               fallback_config.api_key = g_secrets.openai_api_key;
            else if (fallback_config.cloud_provider == CLOUD_PROVIDER_CLAUDE)
               fallback_config.api_key = g_secrets.claude_api_key;
            else if (fallback_config.cloud_provider == CLOUD_PROVIDER_GEMINI)
               fallback_config.api_key = g_secrets.gemini_api_key;
            /* OpenRouter gateway: reroute the fallback (session) provider through
             * OpenRouter too, so it doesn't issue a NULL-key request under gateway.
             * The session model is a direct-API name (claude-sonnet-4-6, not a
             * vendor/model slug) — swap to the OpenRouter extraction model, matching
             * the primary resolver, or OpenRouter would reject the unrecognized id. */
            if (llm_apply_openrouter_gateway(&fallback_config.cloud_provider,
                                             &fallback_config.endpoint, &fallback_config.api_key)) {
               fallback_config.model = g_config.memory.extraction_openrouter_model[0]
                                           ? g_config.memory.extraction_openrouter_model
                                           : llm_get_default_openrouter_model();
            }
         }

         response = llm_chat_completion_with_config(extraction_history, prompt, NULL, NULL, 0,
                                                    &fallback_config);
         if (response) {
            used_fallback = true;
         }
      }
   }

   json_object_put(extraction_history);

   /* Recovery-triggered extractions process old, idle conversations the user
    * has long since moved on from.  Surfacing a noisy "extraction failed"
    * toast for those is just clutter — log only, don't notify. */
   bool is_recovery_run = (strncmp(ctx->session_id, "recovery_", 9) == 0);

   /* Compute source range for provenance: (last_extracted_msg_id + 1, MAX(messages.id)).
    * Queried here — after LLM returns — to avoid a race with concurrent inserts. */
   memory_provenance_t prov = { 0 }; /* zeroed = no provenance */
   if (ctx->conversation_id > 0 && response) {
      int64_t last_id = 0;
      memory_db_get_last_extracted_msg_id(ctx->conversation_id, &last_id);
      int64_t max_id = 0;
      conv_db_get_max_msg_id(ctx->conversation_id, ctx->user_id, &max_id);
      if (max_id > 0 && max_id >= last_id) {
         prov.conv_id = ctx->conversation_id;
         prov.msg_id_start = last_id + 1;
         prov.msg_id_end = max_id;
      }
   }

   if (response) {
      process_extraction_response(ctx->user_id, ctx->conversation_id, ctx->session_id, response,
                                  ctx->new_message_count, ctx->duration_seconds,
                                  prov.conv_id > 0 ? &prov : NULL);
      free(response);

      /* Notify user if we had to fall back */
      if (used_fallback) {
         char notice[256];
         snprintf(notice, sizeof(notice),
                  "Memory extraction used fallback model \"%s\" "
                  "(configured model \"%s\" unavailable)",
                  ctx->fallback.model,
                  g_config.memory.extraction_model[0] ? g_config.memory.extraction_model
                                                      : "(default)");
         OLOG_WARNING("memory_extraction: %s", notice);
#ifdef ENABLE_WEBUI
         if (!is_recovery_run) {
            webui_broadcast_memory_notice(ctx->user_id, "warning", notice);
         }
#endif
      }

      /* Update extraction high-water mark on success.  Pass prov.msg_id_end so
       * the cursor is set to the value captured *before* LLM inference, not a
       * re-queried MAX that may have advanced during the call. */
      if (ctx->conversation_id > 0) {
         memory_db_set_last_extracted(ctx->conversation_id, ctx->message_count, prov.msg_id_end);
         OLOG_INFO("memory_extraction: updated high-water mark to %d (msg_id=%lld) for conv %ld",
                   ctx->message_count, (long long)prov.msg_id_end, (long)ctx->conversation_id);
      }

      /* Run fact pruning if enabled */
      if (g_config.memory.pruning_enabled) {
         int pruned_superseded = 0;
         memory_db_fact_prune_superseded(ctx->user_id, g_config.memory.prune_superseded_days,
                                         &pruned_superseded);
         int pruned_stale = 0;
         memory_db_fact_prune_stale(ctx->user_id, g_config.memory.prune_stale_days,
                                    g_config.memory.prune_stale_min_confidence, &pruned_stale);
         if (pruned_superseded > 0 || pruned_stale > 0) {
            OLOG_INFO("memory_extraction: pruned %d superseded, %d stale facts for user %d",
                      pruned_superseded, pruned_stale, ctx->user_id);
         }
      }
   } else {
      OLOG_WARNING("memory_extraction: LLM returned no response (model=%s, conv=%ld)",
                   g_config.memory.extraction_model[0] ? g_config.memory.extraction_model
                                                       : "(default)",
                   (long)ctx->conversation_id);

      /* Distinguish transient (cloud unreachable, pre-flight failure) from
       * genuine LLM failures.  The recovery/reextract orchestrator reads
       * this signal via memory_extraction_consume_last_transient() to
       * decide whether to roll back the attempt counter -- without it,
       * network blips during a long reextract loop would shelve
       * conversations after max_attempts hits even though the LLM never
       * actually rejected them.  Outcome stamp happens unconditionally
       * just before cleanup so the success path also clears any stale
       * transient flag from a prior extraction on the same user.  Use
       * primary_was_transient OR current llm_last_error() so the signal
       * survives a non-transient fallback failure overwriting it. */
      bool final_was_transient = (primary_was_transient ||
                                  llm_last_error() == LLM_ERR_TRANSIENT_NETWORK);
      if (final_was_transient) {
         OLOG_INFO("memory_extraction: failure was transient (cloud unreachable) for "
                   "conv %ld -- caller should roll back attempt counter",
                   (long)ctx->conversation_id);
      }
#ifdef ENABLE_WEBUI
      if (!is_recovery_run) {
         webui_broadcast_memory_notice(ctx->user_id, "error",
                                       "Memory extraction failed for this session - see daemon "
                                       "logs for details.");
      }
#endif
   }

   /* Stamp this extraction's outcome for the recovery worker to consume.
    * Done unconditionally so a successful run clears any stale transient
    * flag from a prior failed extraction on the same user.  The TLS read
    * (llm_last_error / primary_was_transient capture) happens from the
    * same thread that produced the value, so no synchronization needed
    * for the read; the store into s_last_outcome takes the mutex. */
   {
      bool was_transient = (response == NULL && (primary_was_transient ||
                                                 llm_last_error() == LLM_ERR_TRANSIENT_NETWORK));
      pthread_mutex_lock(&s_extraction_mutex);
      set_last_outcome_locked(ctx->user_id, was_transient);
      pthread_mutex_unlock(&s_extraction_mutex);
   }

   free(prompt);

cleanup:
   free(existing_profile);
   free(ctx->conversation_json);

   /* Clear in-progress flag - save user_id before freeing ctx */
   int saved_user_id = ctx->user_id;
   free(ctx);

   pthread_mutex_lock(&s_extraction_mutex);
   extraction_slot_release_locked(saved_user_id);
   pthread_mutex_unlock(&s_extraction_mutex);

   OLOG_INFO("memory_extraction: completed for user %d", saved_user_id);
   return NULL;
}

/* Return a message for the extraction transcript with inline base64 image data
 * replaced by a "[image]" text placeholder.  Vision turns store content as an OpenAI
 * multi-part array ({type:"image_url",image_url:{url:"data:image/...;base64,..."}});
 * serializing that verbatim into the extraction prompt injects ~250 KB of base64 per
 * image, overflowing the model context (observed: ~333K-token payload → HTTP 400) for
 * zero extraction value — extraction mines text, not pixels.
 *
 * Never mutates @p msg (it is a live reference into the session's conversation history):
 * builds a NEW object only when stripping is needed, copying top-level keys and sharing
 * immutable child refs.  Returns an owned (+1) reference in every case, so the caller
 * adds it to the array unconditionally. */
static struct json_object *extraction_message_strip_images(struct json_object *msg) {
   struct json_object *content = NULL;
   if (!json_object_object_get_ex(msg, "content", &content) ||
       !json_object_is_type(content, json_type_array)) {
      return json_object_get(msg); /* plain-string content — no images to strip */
   }

   int part_count = json_object_array_length(content);
   bool has_image = false;
   for (int i = 0; i < part_count; i++) {
      struct json_object *part = json_object_array_get_idx(content, i);
      struct json_object *type_obj = NULL;
      if (part != NULL && json_object_object_get_ex(part, "type", &type_obj) &&
          strcmp(json_object_get_string(type_obj), "image_url") == 0) {
         has_image = true;
         break;
      }
   }
   if (!has_image) {
      return json_object_get(msg); /* multi-part but text-only — share untouched */
   }

   struct json_object *clean = json_object_new_object();
   if (clean == NULL) {
      return json_object_get(msg); /* OOM — fall back to original (oversized but valid) */
   }

   json_object_object_foreach(msg, key, val) {
      if (strcmp(key, "content") == 0) {
         struct json_object *clean_content = json_object_new_array();
         for (int i = 0; i < part_count; i++) {
            struct json_object *part = json_object_array_get_idx(content, i);
            struct json_object *type_obj = NULL;
            bool is_image = part != NULL && json_object_object_get_ex(part, "type", &type_obj) &&
                            strcmp(json_object_get_string(type_obj), "image_url") == 0;
            if (is_image) {
               struct json_object *placeholder = json_object_new_object();
               json_object_object_add(placeholder, "type", json_object_new_string("text"));
               json_object_object_add(placeholder, "text", json_object_new_string("[image]"));
               json_object_array_add(clean_content, placeholder);
            } else {
               json_object_array_add(clean_content, json_object_get(part));
            }
         }
         json_object_object_add(clean, "content", clean_content);
      } else {
         json_object_object_add(clean, key, json_object_get(val));
      }
   }
   return clean;
}

/* Per-message content budget for extraction input. A single oversized tool
 * result (observed: a runaway 705 KB graph-query dump) can push the whole
 * extraction payload past the model's context window and fail the request
 * (613K-token payload -> HTTP 400). 16 KB is generous for real conversational
 * content; tool results carry context, not user facts, so truncating them for
 * extraction is safe. */
#define MEMORY_EXTRACTION_MAX_MSG_BYTES 16384

/* Cap a single message's string content to MEMORY_EXTRACTION_MAX_MSG_BYTES.
 * Returns an owned ref: the original when within budget, else a copy with
 * UTF-8-safe-truncated content plus a "[... N bytes truncated]" marker. Array
 * (multimodal) content is not capped here — strip_images already replaces image
 * parts, and an oversized multipart *text* part is user-typed, not the runaway
 * tool-result case this guards (the observed failure class is plain strings). */
static struct json_object *extraction_message_cap_content(struct json_object *msg,
                                                          size_t max_bytes) {
   struct json_object *content = NULL;
   if (!json_object_object_get_ex(msg, "content", &content) ||
       !json_object_is_type(content, json_type_string)) {
      return json_object_get(msg);
   }
   const char *text = json_object_get_string(content);
   size_t len = text != NULL ? strlen(text) : 0;
   if (len <= max_bytes) {
      return json_object_get(msg);
   }

   /* Copy a little past max_bytes so utf8_truncate can trim a split codepoint. */
   size_t copy_len = len < max_bytes + 4 ? len : max_bytes + 4;
   char *buf = malloc(copy_len + 1);
   if (buf == NULL) {
      return json_object_get(msg); /* OOM — keep original (oversized but valid) */
   }
   memcpy(buf, text, copy_len);
   buf[copy_len] = '\0';
   utf8_truncate(buf, max_bytes);

   char marker[96];
   snprintf(marker, sizeof(marker), "\n[... %zu bytes truncated for memory extraction]",
            len - strlen(buf));

   struct json_object *clean = json_object_new_object();
   if (clean == NULL) {
      free(buf);
      return json_object_get(msg);
   }
   json_object_object_foreach(msg, key, val) {
      if (strcmp(key, "content") == 0) {
         size_t need = strlen(buf) + strlen(marker) + 1;
         char *joined = malloc(need);
         if (joined != NULL) {
            snprintf(joined, need, "%s%s", buf, marker);
            json_object_object_add(clean, "content", json_object_new_string(joined));
            free(joined);
         } else {
            json_object_object_add(clean, "content", json_object_new_string(buf));
         }
      } else {
         json_object_object_add(clean, key, json_object_get(val));
      }
   }
   free(buf);
   return clean;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int memory_trigger_extraction(int user_id,
                              int64_t conversation_id,
                              const char *session_id_str,
                              struct json_object *conversation_history,
                              int message_count,
                              int duration_seconds,
                              const memory_extraction_fallback_t *fallback) {
   if (user_id <= 0 || !conversation_history) {
      return 1;
   }

   if (!g_config.memory.enabled) {
      return 1;
   }

#ifdef ENABLE_AUTH
   /* Re-check privacy status from database (prevents race condition with set_private) */
   if (conversation_id > 0) {
      bool is_private = false;
      conv_db_is_private(conversation_id, user_id, &is_private);
      if (is_private) {
         OLOG_INFO("memory_extraction: skipping - conversation %lld is private (DB check)",
                   (long long)conversation_id);
         return 0;
      }
      /* On error, is_private stays false — proceed with extraction */
   }
#endif

   /* Skip if too few messages.  Mark the conversation as up-to-date so
    * recovery scans don't keep re-evaluating a structurally inextractable
    * conversation forever.  If the user later adds a 2nd message the
    * counter advances back to zero on the new live extraction. */
   if (message_count < 2) {
      OLOG_INFO("memory_extraction: skipping - too few messages (%d)", message_count);
      if (conversation_id > 0) {
         memory_db_set_last_extracted(conversation_id, message_count,
                                      0); /* 0 = leave msg_id cursor unchanged */
      }
      return 0;
   }

   /* ID-based incremental extraction cursor.
    * Also maintain the count-based cursor for one release cycle (back-compat
    * verification — lets spot-checking confirm both cursors agree). */
   int last_extracted = 0;
   int64_t last_msg_id = 0;
   if (conversation_id > 0) {
      memory_db_get_last_extracted(conversation_id, &last_extracted);
      memory_db_get_last_extracted_msg_id(conversation_id, &last_msg_id);

      /* ID-based early-skip: compare max DB row ID against cursor. */
      int64_t max_msg_id = 0;
      conv_db_get_max_msg_id(conversation_id, user_id, &max_msg_id);
      if (max_msg_id > 0 && max_msg_id <= last_msg_id) {
         OLOG_INFO("memory_extraction: skipping — no new messages (max_id=%lld, last=%lld)",
                   (long long)max_msg_id, (long long)last_msg_id);
         return 0;
      }
   }

   int new_messages = message_count - last_extracted;

   /* Check concurrency limits and acquire extraction slot */
   pthread_mutex_lock(&s_extraction_mutex);
   if (!extraction_slot_acquire_locked(user_id)) {
      /* Either user already has extraction in progress, or we've hit max concurrent */
      bool user_active = extraction_is_active_locked(user_id);
      int max_concurrent = get_max_concurrent_extractions();
      pthread_mutex_unlock(&s_extraction_mutex);

      if (user_active) {
         OLOG_INFO("memory_extraction: already in progress for user %d", user_id);
      } else {
         OLOG_INFO("memory_extraction: at capacity (%d/%d), skipping user %d",
                   s_extraction_state.count, max_concurrent, user_id);
      }
      return 0;
   }
   pthread_mutex_unlock(&s_extraction_mutex);

   /* Prepare context */
   extraction_context_t *ctx = calloc(1, sizeof(extraction_context_t));
   if (!ctx) {
      pthread_mutex_lock(&s_extraction_mutex);
      extraction_slot_release_locked(user_id);
      pthread_mutex_unlock(&s_extraction_mutex);
      return 1;
   }

   ctx->user_id = user_id;
   ctx->conversation_id = conversation_id;
   if (session_id_str) {
      strncpy(ctx->session_id, session_id_str, MEMORY_SESSION_ID_MAX - 1);
   } else {
      snprintf(ctx->session_id, MEMORY_SESSION_ID_MAX, "session_%ld", (long)time(NULL));
   }
   ctx->message_count = message_count;
   ctx->new_message_count = new_messages;
   ctx->duration_seconds = duration_seconds;
   if (fallback) {
      ctx->has_fallback = true;
      ctx->fallback = *fallback;
   }

   /* Note-extraction guard: collect note-filed bodies + document_read echoes
    * from the FULL history so they can be redacted out of the extraction input
    * (keeps reference text the user filed as a note out of semantic memory).
    * NULL when disabled or nothing was filed → redact is a pass-through. */
   memory_note_guard_t *note_guard = memory_note_guard_create(conversation_history);

   /* ID-based filter: include messages where id > last_msg_id.
    * Messages with missing or zero id (system messages, voice-only path,
    * messages added before this feature shipped) are included unconditionally.
    * System messages are always skipped by the role check. */
   size_t arr_len = json_object_array_length(conversation_history);
   struct json_object *filtered = json_object_new_array();

   for (size_t i = 0; i < arr_len; i++) {
      struct json_object *msg = json_object_array_get_idx(conversation_history, i);
      struct json_object *role_obj, *id_obj;
      if (!json_object_object_get_ex(msg, "role", &role_obj))
         continue;
      if (strcmp(json_object_get_string(role_obj), "system") == 0)
         continue;
      int64_t msg_id = 0;
      if (json_object_object_get_ex(msg, "id", &id_obj))
         msg_id = json_object_get_int64(id_obj);
      if (msg_id == 0 || msg_id > last_msg_id) {
         /* strip_images then guard-redact: both return an owned ref and never
          * mutate the live history; chaining yields one redacted copy (or a
          * shared ref when neither stage changes anything). */
         struct json_object *stripped = extraction_message_strip_images(msg);
         struct json_object *guarded = memory_note_guard_redact(note_guard, stripped);
         json_object_put(stripped);
         struct json_object *capped = extraction_message_cap_content(
             guarded, MEMORY_EXTRACTION_MAX_MSG_BYTES);
         json_object_put(guarded);
         json_object_array_add(filtered, capped);
      }
   }

   memory_note_guard_free(note_guard);

   ctx->conversation_json = strdup(
       json_object_to_json_string_ext(filtered, JSON_C_TO_STRING_PLAIN));
   json_object_put(filtered);

   if (!ctx->conversation_json) {
      free(ctx);
      pthread_mutex_lock(&s_extraction_mutex);
      extraction_slot_release_locked(user_id);
      pthread_mutex_unlock(&s_extraction_mutex);
      return 1;
   }

   OLOG_INFO("memory_extraction: extracting %d new messages (last=%d, total=%d)", new_messages,
             last_extracted, message_count);

   /* Spawn detached extraction thread */
   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

   int rc = pthread_create(&thread, &attr, extraction_thread, ctx);
   pthread_attr_destroy(&attr);

   if (rc != 0) {
      OLOG_ERROR("memory_extraction: failed to create thread: %d", rc);
      free(ctx->conversation_json);
      free(ctx);
      pthread_mutex_lock(&s_extraction_mutex);
      extraction_slot_release_locked(user_id);
      pthread_mutex_unlock(&s_extraction_mutex);
      return 1;
   }

   OLOG_INFO("memory_extraction: triggered for user %d", user_id);
   return 0;
}

bool memory_extraction_in_progress(int user_id) {
   if (user_id <= 0) {
      return false;
   }

   pthread_mutex_lock(&s_extraction_mutex);
   bool in_progress = extraction_is_active_locked(user_id);
   pthread_mutex_unlock(&s_extraction_mutex);

   return in_progress;
}

bool memory_extraction_consume_last_transient(int user_id) {
   if (user_id <= 0) {
      return false;
   }
   pthread_mutex_lock(&s_extraction_mutex);
   bool result = (s_last_outcome.last_user_id == user_id && s_last_outcome.was_transient);
   if (result) {
      /* Clear so a subsequent consume returns false — the recovery
       * worker reads once per extraction it triggered. */
      s_last_outcome.was_transient = false;
   }
   pthread_mutex_unlock(&s_extraction_mutex);
   return result;
}

size_t memory_extraction_get_template_size_chars(void) {
   return strlen(MEMORY_EXTRACTION_PROMPT_TEMPLATE);
}

int memory_extraction_resolve_config(llm_resolved_config_t *cfg,
                                     char *model_buf,
                                     size_t model_buf_sz,
                                     char *endpoint_buf,
                                     size_t endpoint_buf_sz,
                                     const char *log_prefix) {
   const char *prefix = log_prefix ? log_prefix : "memory_extraction";
   if (!cfg || !model_buf || !endpoint_buf) {
      OLOG_ERROR("%s: NULL argument to resolve_config", prefix);
      return FAILURE;
   }
   if (model_buf_sz < LLM_MODEL_NAME_MAX) {
      OLOG_ERROR("%s: model_buf_sz %zu < required %d", prefix, model_buf_sz, LLM_MODEL_NAME_MAX);
      return FAILURE;
   }
   if (endpoint_buf_sz < MEMORY_EXTRACTION_ENDPOINT_BUF_MIN) {
      OLOG_ERROR("%s: endpoint_buf_sz %zu < required %d", prefix, endpoint_buf_sz,
                 MEMORY_EXTRACTION_ENDPOINT_BUF_MIN);
      return FAILURE;
   }
   memset(cfg, 0, sizeof(*cfg));

   const char *provider = g_config.memory.extraction_provider;
   const char *model = g_config.memory.extraction_model;

   if (!provider || provider[0] == '\0') {
      OLOG_ERROR("%s: extraction_provider not configured", prefix);
      return FAILURE;
   }

   if (model && model[0] != '\0') {
      strncpy(model_buf, model, model_buf_sz - 1);
      model_buf[model_buf_sz - 1] = '\0';
      cfg->model = model_buf;
   }

   if (strcmp(provider, "local") == 0 || strcmp(provider, "ollama") == 0) {
      cfg->type = LLM_LOCAL;
      cfg->cloud_provider = CLOUD_PROVIDER_NONE;
      strncpy(endpoint_buf, g_config.llm.local.endpoint, endpoint_buf_sz - 1);
      endpoint_buf[endpoint_buf_sz - 1] = '\0';
      cfg->endpoint = endpoint_buf;
   } else if (strcmp(provider, "openai") == 0) {
      cfg->type = LLM_CLOUD;
      cfg->cloud_provider = CLOUD_PROVIDER_OPENAI;
      cfg->api_key = g_secrets.openai_api_key;
      cfg->endpoint = NULL;
   } else if (strcmp(provider, "claude") == 0) {
      cfg->type = LLM_CLOUD;
      cfg->cloud_provider = CLOUD_PROVIDER_CLAUDE;
      cfg->api_key = g_secrets.claude_api_key;
      cfg->endpoint = NULL;
   } else {
      OLOG_WARNING("%s: unknown provider '%s' in config "
                   "(valid: local, ollama, openai, claude) - falling back to local",
                   prefix, provider);
      cfg->type = LLM_LOCAL;
      cfg->cloud_provider = CLOUD_PROVIDER_NONE;
      strncpy(endpoint_buf, g_config.llm.local.endpoint, endpoint_buf_sz - 1);
      endpoint_buf[endpoint_buf_sz - 1] = '\0';
      cfg->endpoint = endpoint_buf;
   }

   /* OpenRouter gateway: reroute cloud extraction through OpenRouter.  Under the gateway,
    * swap in the OpenRouter-formatted extraction model (vendor/model), falling back to the
    * main OpenRouter default when unset — the direct extraction_model naming would not
    * resolve on OpenRouter. */
   if (llm_apply_openrouter_gateway(&cfg->cloud_provider, &cfg->endpoint, &cfg->api_key)) {
      const char *or_model = g_config.memory.extraction_openrouter_model[0]
                                 ? g_config.memory.extraction_openrouter_model
                                 : llm_get_default_openrouter_model();
      strncpy(model_buf, or_model, model_buf_sz - 1);
      model_buf[model_buf_sz - 1] = '\0';
      cfg->model = model_buf;
   }

   strncpy(cfg->tool_mode, "disabled", sizeof(cfg->tool_mode) - 1);
   strncpy(cfg->thinking_mode, "disabled", sizeof(cfg->thinking_mode) - 1);
   cfg->timeout_ms = g_config.memory.extraction_timeout_ms;

   return SUCCESS;
}
