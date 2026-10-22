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
 * Memory Tool - Persistent memory for facts and preferences
 *
 * Supports: remember (store), search (query), forget (delete), recent (list)
 * Implementation is in memory/memory_callback.c, this file handles tool registration.
 */

#include "tools/memory_tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_types.h" /* MEMORY_FACT_CATEGORIES */
#include "tools/tool_registry.h"
#include "utils/string_utils.h"

/* Fact ids a forget's confirm text looks at (the tool takes at most 50). */
#define MEMORY_FORGET_DESCRIBE_IDS_MAX 50

/* Forward declaration of callback from memory/memory_callback.c */
char *memoryCallback(const char *actionName, char *value, int *should_respond);

/* Forward declaration for availability check */
static bool memory_tool_is_available(void);

/* ========== Tool Parameter Definition ========== */

static const treg_param_t memory_params[] = {
   {
       .name = "action",
       .description =
           "Memory action: 'remember' (store a fact), 'search' (find memories), "
           "'get' (retrieve specific memories EXACTLY by numeric ID — the deterministic "
           "counterpart to 'search'; use when you already know the ID(s) and want the exact "
           "stored text rather than a relevance-ranked match), "
           "'forget' (remove memories by numeric ID — DELETE permanently, MERGE duplicates "
           "into one kept with 'duplicate_of', or file older values a newer one replaced with "
           "'newer_fact_id'; use search/recent/find_duplicates first to find the IDs), 'recent' "
           "(list recent memories), "
           "'find_duplicates' (list clusters of near-identical facts with their IDs so you "
           "can forget the redundant ones), "
           "'list_merged' (memories merged into another or replaced by a newer one, which "
           "recall no longer shows), 'restore' (bring one of those back by ID, when the user "
           "says a merge or replacement was wrong), "
           "'save_contact' (store email/phone for a person), "
           "'find_contact' (look up contact info), "
           "'list_contacts' (list all contacts), "
           "'delete_contact' (remove contact by ID), "
           "'merge_entities' (soft-link two entities that refer to the same person/thing — "
           "creates a reversible alias; both entity rows remain)",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "remember", "search", "get", "forget", "recent", "find_duplicates",
                        "list_merged", "restore", "save_contact", "find_contact", "list_contacts",
                        "delete_contact", "merge_entities" },
       .enum_count = 13,
   },
   {
       .name = "query",
       .description =
           "POLYMORPHIC — the expected SHAPE of this value depends on the chosen action. "
           "Check the action you picked before filling this in:\n"
           "  remember:        STRING — one fact, e.g. 'User prefers dark mode'. To store "
           "SEVERAL facts at once in a single call, pass a JSON array instead "
           /* "max 25" mirrors MAX_REMEMBER_FACTS (src/memory/memory_callback.c) — keep in sync. */
           "(max 25). Each element is EITHER a plain string (a DURABLE fact) OR an object "
           "{\"text\": \"...\", \"expires_at\": \"YYYY-MM-DD\"} for a TRANSIENT dated fact "
           "that loses value after that date — a movie/event on a specific day, a weather "
           "forecast, an appointment. Set expires_at ONLY for such facts; use a plain string "
           "for anything durable (preferences, traits, relationships, lasting records). Mix "
           "freely, e.g. [\"User prefers dark mode\", {\"text\": \"Movie The Odyssey on "
           "2026-07-22 7:00 PM\", \"expires_at\": \"2026-07-22\"}].\n"
           "  search:          STRING — keywords (3-8 words work best).\n"
           "  get:             NUMERIC ID(s) — one ID, or a comma-separated list, e.g. '7858' "
           "or '7858,7854'. Returns those exact memories verbatim (no ranking); non-numeric "
           "tokens are ignored.\n"
           "  forget:          NUMERIC ID(s) — one fact ID, or a comma-separated list to "
           "bulk-delete, e.g. '4711' or '4711,4712,4713' (max 50). IDs come from a prior "
           "search/recent/find_duplicates result; non-numeric tokens are ignored.\n"
           "  find_duplicates: OPTIONAL NUMERIC — cosine similarity threshold 0.50-1.00 "
           "(default 0.85). Omit to use the default; raise it for stricter (more-identical) "
           "matches. To clean up, pass the redundant IDs to 'forget' (max 50 per call — "
           "split larger sets across calls).\n"
           "  list_merged:     OPTIONAL — a number alone is a fact ID and lists what was "
           "merged into that fact; words list merged or replaced memories containing them; "
           "omit for the newest.\n"
           "  restore:         NUMERIC ID — one merged or replaced memory from list_merged.\n"
           "  recent:          TIME WINDOW — how far back to look. Suffix-encoded: "
           "'24h', '7d', '2w', '90d', '1y', '10y' (default '7d', upper bound '10y'). "
           "Returns entries within [now − query, now]. Use 'sort' and 'before' params for "
           "ordering and upper-bound.\n"
           "  save_contact:    STRING — the person's name (e.g. 'Jane Doe').\n"
           "  find_contact:    STRING — name (or partial name) to search for.\n"
           "  list_contacts:   OPTIONAL STRING — field_type filter, one of 'email' or "
           "'phone'. Omit to list all contacts.\n"
           "  delete_contact:  NUMERIC STRING — contact ID from a prior find_contact / "
           "list_contacts result.\n"
           "  merge_entities:  STRING — the SOURCE entity name (will become an alias of "
           "the entity named in 'target_name'). Both names must already exist as entities; "
           "run 'search' first to verify.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
   {
       .name = "time_range",
       .description = "Optional time filter for 'search'. Limit results to memories from this "
                      "period back to now. Examples: '24h', '7d', '2w', '30d'. Only used "
                      "with 'search'.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "time_range",
   },
   /* Bundle 3 (2026-05-13) — windowed / sorted / count-controlled retrieval
    * for 'recent'.  The pre-Bundle-3 tool surface offered no way for the
    * LLM to ask "what's my earliest memory" (every SELECT was DESC-only),
    * no result-count control (hardcoded 20-fact / 10-summary caps), and
    * no time-window with both bounds.  These three additions close those
    * gaps.  'search' has its own hybrid-scoring + top-N pattern and is
    * not affected by these params. */
   {
       .name = "limit",
       .description = "Optional result count for 'recent' (max per category — facts and "
                      "summaries each capped to this). Range 1-50; default 20 facts / "
                      "10 summaries. Not used by 'search' (which returns its hybrid top "
                      "hits).",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "limit",
   },
   {
       .name = "sort",
       .description = "Optional sort order for 'recent'. 'newest' (default) returns the most "
                      "recent entries first; 'oldest' returns the earliest entries first. "
                      "Sort applies ONLY WITHIN the time window set by 'query' — it does "
                      "not change how far back 'recent' reaches. To find the earliest "
                      "entries in the entire store, widen 'query' (its upper bound is "
                      "'10y'). Not used by 'search' (its hybrid scoring drives ordering).",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "sort",
       .enum_values = { "newest", "oldest" },
       .enum_count = 2,
   },
   {
       .name = "before",
       .description = "Optional time-period upper bound for 'recent' (e.g., '3d', '30d'). "
                      "Restricts results to entries older than 'before' ago. Combine with "
                      "'query' (the lower bound) to query a slice in the past — query and "
                      "before together bracket a window [now − query, now − before]. Not "
                      "used by 'search'.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "before",
   },
   {
       .name = "target_name",
       .description = "For 'merge_entities': the name of the entity to make canonical (keeps its "
                      "name and identity; the source becomes a soft alias that resolves here).",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "target_name",
   },
   /* v34 — fact-category pre-filter for retrieval.  Mirrors the canonical
    * MEMORY_FACT_CATEGORIES taxonomy in memory_db.c.  Inline duplication is
    * intentional: tool registry needs compile-time enum_values, and centralizing
    * it via a runtime pointer would force a registry-shape change. */
   {
       .name = "category",
       .description = "Optional topic tag. No longer narrows 'search' results — semantic "
                      "ranking covers all categories, so you do NOT need to guess a category "
                      "to find a fact. Accepted for back-compat but safe to omit.",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "category",
       .enum_values = { "personal", "professional", "relationships", "health", "interests",
                        "practical", "preferences", "general" },
       .enum_count = 8,
   },
   /* v33 — relation validity at a historical timestamp. */
   {
       .name = "as_of",
       .description = "Optional ISO date (YYYY-MM-DD or YYYY) for 'search'. Adds the "
                      "relations of the people and things the query names that were valid at "
                      "that time (e.g., 'who did Alice work for in 2020').",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "as_of",
   },
   {
       .name = "include_historical",
       .description = "Set to true for 'search' to add the relations of the people and things "
                      "the query names, including ended ones (a past job or home). Without "
                      "this or as_of, search returns no relations.",
       .type = TOOL_PARAM_TYPE_BOOL,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "include_historical",
   },
   /* Source-linked recall: verbatim conversation excerpts for the top facts, so
    * the caller can verify the extractor's paraphrase. */
   {
       .name = "with_source",
       .description = "Set to true to include verbatim source excerpts for the top few facts "
                      "returned by 'search' or 'recent', within a shared character budget. "
                      "Facts stored before sources were kept, and facts from private "
                      "conversations, have none. For more, call context_expand.",
       .type = TOOL_PARAM_TYPE_BOOL,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "with_source",
   },
   /* Private-conversation confirmation gate for 'remember'.  Not a blanket block:
    * a user can legitimately say "remember X" inside a private conversation, and
    * silently dropping that is a worse failure than the leak it would prevent —
    * they believe it was saved.  What must not happen is the MODEL deciding on
    * its own to persist private material.  The tool cannot tell those apart, so
    * it asks. */
   {
       .name = "confirm_private",
       .description =
           "For 'remember' in a PRIVATE conversation ONLY.  Set this to true when THE USER asked "
           "for the save — if they said 'remember that', 'save this', 'note that down', or "
           "similar, they have already decided and you should just set it and save.  Do NOT ask "
           "them to confirm something they only just told you to do.  Leave it unset when saving "
           "is YOUR idea rather than theirs; the tool will then decline and you should ask first. "
           "The distinction is who decided, not how sensitive the content looks.",
       .type = TOOL_PARAM_TYPE_BOOL,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "confirm_private",
   },
   /* 'forget' with a kept fact: supersede the listed IDs into it instead of deleting. */
   {
       .name = "duplicate_of",
       .description =
           "For 'forget' ONLY: the fact ID you are KEEPING when the IDs being forgotten say the "
           "SAME thing (duplicates). Turns 'forget' into a MERGE: they are hidden from recall but "
           "stay recoverable, instead of being deleted. Use it when the user says 'merge' or you "
           "clean a find_duplicates cluster. For a value that CHANGED, use 'newer_fact_id'.",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "duplicate_of",
   },
   {
       .name = "newer_fact_id",
       .description =
           "For 'forget' ONLY: the fact ID of the NEWER value when the IDs being forgotten are "
           "OLDER values it replaced (a new phone number or address, a new job: the user said it "
           "changed). They are hidden from recall but kept as history, and come back if the newer "
           "fact is forgotten (it was wrong). Coarse: everything an old fact said is closed. For "
           "duplicates use 'duplicate_of'. Omit both only when the facts should be gone for good.",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "newer_fact_id",
   },
   /* duplicate_of's earlier name: a conversation keeps the tool definition it
    * started with, and its turns may still send it.  Declared so it reaches the
    * callback (only declared parameters do), which treats it as duplicate_of. */
   {
       .name = "replaced_by",
       .description = "Older name for 'duplicate_of' (same meaning). Use 'duplicate_of'.",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "replaced_by",
   },
   /* The contact fields: save_contact reads all four, find_contact and
    * list_contacts read field_type. */
   {
       .name = "field_type",
       .description = "For 'save_contact' / 'find_contact' / 'list_contacts': the kind of "
                      "contact info, e.g. 'email', 'phone' or 'address'.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "field_type",
   },
   {
       .name = "value",
       .description = "For 'save_contact': the address, number or other contact info itself.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "value",
   },
   {
       .name = "label",
       .description = "For 'save_contact': an optional label, e.g. 'work' or 'personal'.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "label",
   },
   {
       .name = "entity_id",
       .description = "For 'save_contact': the person's entity ID, when a previous call asked you "
                      "to choose between similar people.",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "entity_id",
   },
};

/* ========== Tool Metadata ========== */

static const tool_action_kind_entry_t s_memory_action_kinds[] = {
   { "search", TOOL_KIND_READ, NULL },
   { "get", TOOL_KIND_READ, NULL },
   { "recent", TOOL_KIND_READ, NULL },
   { "find_duplicates", TOOL_KIND_READ, NULL },
   { "find_contact", TOOL_KIND_READ, NULL },
   { "list_contacts", TOOL_KIND_READ, NULL },
   { "list_merged", TOOL_KIND_READ, NULL },
   /* Change what recall returns (forget can't be undone): acts, so they wait
    * for the user where a turn is someone else's (an attached email, an
    * unverified sender); a fetched email or page doesn't make it one yet. */
   { "restore", TOOL_KIND_ACT, NULL },
   { "forget", TOOL_KIND_ACT, NULL },
};

/* 'restore' by the text of the fact it brings back: an id means nothing to
 * the person confirming.  TOOL_DESCRIBE_DEFAULT when there is no such fact
 * (the call then fails on its own). */
static int describe_restore(const char *value, char *out, size_t out_len) {
   char id_text[32] = "";
   tool_param_extract_base(value, id_text, sizeof(id_text));
   char *end = NULL;
   const long long id = strtoll(id_text, &end, 10);
   memory_fact_t fact;
   if (id <= 0 || !end || *end != '\0' ||
       memory_db_fact_get((int64_t)id, tool_get_current_user_id(), &fact) != MEMORY_DB_SUCCESS) {
      return TOOL_DESCRIBE_DEFAULT;
   }
   char shown[400];
   str_excerpt_line(fact.fact_text, 300, shown, sizeof(shown));
   const int n = snprintf(out, out_len, "restore: \"%s\"", shown);
   return (n > 0 && (size_t)n < out_len) ? SUCCESS : FAILURE;
}

/* Whether @p name carries a kept fact's id: absent, empty and 0 mean none,
 * as the callback reads them. */
static bool keeper_given(const char *value, const char *name, int64_t *id_out) {
   char v[32] = "";
   if (!tool_param_extract_custom(value, name, v, sizeof(v)) || !v[0] || strcmp(v, "0") == 0) {
      return false;
   }
   *id_out = strtoll(v, NULL, 10);
   return true;
}

/* What a forget of @p ids takes beyond them and brings back, summed over the
 * ids (the confirm's numbers): merged duplicates, values back, the first. */
static void forget_preview(const int64_t *ids,
                           int n,
                           int user_id,
                           int *merged,
                           int *back,
                           int64_t *first_back) {
   *merged = 0;
   *back = 0;
   *first_back = 0;
   for (int i = 0; i < n; i++) {
      int m = 0;
      int r = 0;
      int64_t first = 0;
      if (memory_db_fact_forget_preview(ids[i], user_id, &m, &r, &first) == MEMORY_DB_SUCCESS) {
         *merged += m;
         *back += r;
         if (!*first_back) {
            *first_back = first;
         }
      }
   }
}

/* 'forget' as the person confirming needs it: ids alone mean nothing, so the
 * first fact's text and how many more; for a merge or replace, the fact kept
 * (it becomes the current value); for a delete, the duplicates that go with
 * it and the older values that come back.  TOOL_DESCRIBE_DEFAULT when the
 * first id isn't a fact. */
static int describe_forget(const char *value, char *out, size_t out_len) {
   char list[1024] = "";
   tool_param_extract_base(value, list, sizeof(list));
   int64_t ids[MEMORY_FORGET_DESCRIBE_IDS_MAX];
   int n = 0;
   for (char *p = list; *p && n < MEMORY_FORGET_DESCRIBE_IDS_MAX;) {
      char *end = NULL;
      const long long v = strtoll(p, &end, 10);
      if (end == p) {
         p++;
         continue;
      }
      if (v > 0) {
         ids[n++] = (int64_t)v;
      }
      p = end;
   }
   const int user_id = tool_get_current_user_id();
   memory_fact_t fact;
   if (n == 0 || memory_db_fact_get(ids[0], user_id, &fact) != MEMORY_DB_SUCCESS) {
      return TOOL_DESCRIBE_DEFAULT;
   }
   char text[200];
   str_excerpt_line(fact.fact_text, 150, text, sizeof(text));
   char others[32] = "";
   if (n > 1) {
      snprintf(others, sizeof(others), " and %d more", n - 1);
   }
   int64_t keeper = 0;
   const bool merge = keeper_given(value, "duplicate_of", &keeper) ||
                      keeper_given(value, "replaced_by", &keeper);
   const bool replace = !merge && keeper_given(value, "newer_fact_id", &keeper);
   int w;
   if (merge || replace) {
      memory_fact_t kept;
      char kept_text[200] = "?";
      if (memory_db_fact_get(keeper, user_id, &kept) == MEMORY_DB_SUCCESS) {
         str_excerpt_line(kept.fact_text, 150, kept_text, sizeof(kept_text));
      }
      w = snprintf(out, out_len, "%s \"%s\"%s, keeping \"%s\"",
                   merge ? "merge as a duplicate" : "file as an older value", text, others,
                   kept_text);
   } else {
      int merged = 0;
      int back = 0;
      int64_t first_back = 0;
      forget_preview(ids, n, user_id, &merged, &back, &first_back);
      char extra[64] = "";
      if (merged > 0) {
         snprintf(extra, sizeof(extra), ", with %d merged duplicate%s", merged,
                  merged == 1 ? "" : "s");
      }
      char back_text[200] = "";
      memory_fact_t bf;
      if (back > 0 && memory_db_fact_get(first_back, user_id, &bf) == MEMORY_DB_SUCCESS) {
         str_excerpt_line(bf.fact_text, 120, back_text, sizeof(back_text));
      }
      char more_back[32] = "";
      if (back > 1) {
         snprintf(more_back, sizeof(more_back), " and %d more", back - 1);
      }
      w = snprintf(out, out_len, "forget \"%s\"%s%s%s%s%s%s", text, others, extra,
                   back_text[0] ? "; brings back \"" : "", back_text, back_text[0] ? "\"" : "",
                   back_text[0] ? more_back : "");
   }
   return (w > 0 && (size_t)w < out_len) ? SUCCESS : FAILURE;
}

/* What a call waiting for the user's reply code does: 'remember' and
 * 'restore' by the fact (tool_metadata_t describe_call); the rest by their
 * parameters. */
static int memory_describe_call(const char *action,
                                const char *value,
                                char *out,
                                size_t out_len,
                                int *valid_for_sec) {
   (void)valid_for_sec;
   if (strcmp(action, "restore") == 0) {
      return describe_restore(value, out, out_len);
   }
   if (strcmp(action, "forget") == 0) {
      return describe_forget(value, out, out_len);
   }
   if (strcmp(action, "remember") != 0) {
      return TOOL_DESCRIBE_DEFAULT;
   }
   char fact[MEMORY_FACT_TEXT_MAX * 2] = "";
   tool_param_extract_base(value, fact, sizeof(fact));
   char shown[400];
   str_excerpt_line(fact, 300, shown, sizeof(shown));
   if (!shown[0]) {
      return FAILURE;
   }
   const int n = snprintf(out, out_len, "remember: \"%s\"", shown);
   return (n > 0 && (size_t)n < out_len) ? SUCCESS : FAILURE;
}

static const tool_metadata_t memory_metadata = {
   .name = "memory",
   .action_kinds = s_memory_action_kinds,
   .action_kind_count = TOOL_KIND_COUNT(s_memory_action_kinds),
   .device_string = "memory",
   .topic = "dawn",
   /* 'recall' was an alias here; it is now the dedicated unified cross-source
    * recall tool (recall_tool.c).  Registry rejects duplicate names/aliases,
    * so the alias was removed.  'remember' stays. */
   .aliases = { "remember" },
   .alias_count = 1,
   .describe_call = memory_describe_call,

   .description = "Store and retrieve persistent memories about the user. "
                  "Use 'remember' to store facts (preferences, information shared by user). "
                  "When the user shares something to remember, note, or 'keep in mind' — even "
                  "casually or as an aside — you MUST call 'remember' in the SAME turn. "
                  "Acknowledging it in your reply ('got it', 'noted', 'I'll remember that') does "
                  "NOT store anything; only the tool call persists it. Never claim you saved or "
                  "will remember something without actually calling 'remember'. "
                  /* "511" mirrors MEMORY_FACT_TEXT_MAX - 1 (include/memory/memory_types.h). */
                  "Each fact is ONE concise statement, max 511 characters. To store several "
                  "facts at once, pass a JSON array (one fact per element) — preferred over many "
                  "separate calls for a multi-fact dump. Each element is either a plain string (a "
                  "durable fact) or, for a TRANSIENT dated fact that loses value after a date — a "
                  "movie/event on a day, a forecast, an appointment — an object "
                  "{\"text\": \"...\", \"expires_at\": \"YYYY-MM-DD\"}; use the object form "
                  "whenever a fact is tied to a passing date. For "
                  "longer content (e.g. a journal entry) split it into several concise facts. "
                  "Memory is for facts learned in passing. For verbatim, authored, or living "
                  "reference text the user maintains and retrieves exactly — a bio, a pitch, an "
                  "address, a saved answer — use document_manage (save_note / edit) instead, NOT "
                  "remember. "
                  "Call 'remember' directly; it cannot be used inside execute_plan. "
                  "Use 'search' for a TARGETED lookup of stored memory facts (optionally filtered "
                  "by time_range like '24h', '7d', '2w'). For a BROAD 'what do we know about X' / "
                  "'how do things stand' question, call the 'recall' tool FIRST instead — it spans "
                  "memory, notes, documents, and the calendar in one pass; reach for memory "
                  "'search' when you specifically need memory facts and recall isn't warranted. "
                  "Use 'forget' to remove memories by numeric ID (you MUST use 'search', 'recent', "
                  "or 'find_duplicates' first to find the ID); pass a comma-separated list for "
                  "several. 'forget' has THREE modes — pick deliberately: (1) DELETE (default, "
                  "permanent) for facts that are wrong or unwanted; (2) MERGE (recoverable) for "
                  "duplicates — when the user asks to merge/consolidate/dedupe while keeping one, "
                  "OR you are cleaning duplicates, set 'duplicate_of' to the keeper's ID; (3) "
                  "REPLACE for an older value the user says changed (new number, moved, new job): "
                  "set 'newer_fact_id' to the newer fact's ID, so the old one is kept as history "
                  "and comes back if the newer one turns out wrong. Forgetting a value brings "
                  "back the one it replaced (the reply says which): tell the user, and forget "
                  "that one too only if the user says to. Prefer MERGE or REPLACE over DELETE so a "
                  "wrong call is reversible. "
                  "Use 'recent' to list recently stored memories. "
                  "Use 'save_contact' to store contact info (email, phone) for a person "
                  "(query: person name, field_type: email/phone, value: the address/number, "
                  "label: work/personal, entity_id: optional, use to select a specific person "
                  "when prompted for disambiguation). "
                  "Use 'find_contact' to look up contact info by name "
                  "(query: name, optional field_type). "
                  "Use 'list_contacts' to list all stored contacts (query: optional field_type). "
                  "Use 'delete_contact' to remove a contact record by ID. "
                  "Use 'merge_entities' to soft-link two entities that refer to the same "
                  "person/pet/place (query: source name, target_name: canonical entity to keep). "
                  "The link is reversible — the source row is preserved and can be split later. "
                  "Use with_source=true on 'search'/'recent' to include verbatim conversation "
                  "excerpts for the top few facts. "
                  "Memories persist across sessions and are private to each user.",
   .params = memory_params,
   .param_count = TOOL_PARAM_COUNT(memory_params),

   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_FILESYSTEM,
   .skip_followup = false,
   .default_remote = true,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL, /* Uses [memory] section from main config */

   .is_available = memory_tool_is_available,

   .init = NULL,
   .cleanup = NULL,
   .callback = memoryCallback, /* Use existing callback from memory_callback.c */
};

/* ========== Availability Check ========== */

static bool memory_tool_is_available(void) {
   return g_config.memory.enabled;
}

/* ========== Public API ========== */

int memory_tool_register(void) {
   return tool_registry_register(&memory_metadata);
}
