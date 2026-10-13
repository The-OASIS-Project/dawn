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
 * The memory tool's 'forget': delete facts, merge duplicates into the one kept
 * (duplicate_of), or file older values a newer fact replaced (newer_fact_id).
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db_withdraw.h"
#include "core/session_prefix.h"
#include "core/strbuf.h"
#include "memory/memory_callback_internal.h"
#include "memory/memory_db.h"
#include "memory/memory_extraction.h"
#include "tools/tool_registry.h"

/* How long a forget from the memory tool waits for the user's running
 * extraction: a turn can't stall on it (the WebUI's own forget waits longer,
 * on a worker). */
#define FORGET_TOOL_WAIT_SEC 5

/* Values a forget call names as current again (forget_reply). */
#define FORGET_BACK_MAX 8

/* @p msg (taken) with what forgetting brought back (@p back, freed). */
static char *forget_reply(char *msg, strbuf_t *back) {
   if (!msg || back->len == 0) {
      strbuf_free(back);
      return msg;
   }
   strbuf_t out;
   strbuf_init(&out, strlen(msg) + back->len + 160);
   strbuf_append(&out, msg);
   free(msg);
   if (back->len > 0) {
      strbuf_appendf(&out,
                     ". Current again, as it was before what was forgotten replaced it: %s. Tell "
                     "the user these are back; forget them only if the user says to",
                     back->buf);
   }
   strbuf_free(back);
   char *result = strbuf_steal(&out);
   return result ? result : strdup("Memory updated.");
}

/* Parse one or more comma/space-separated POSITIVE numeric fact IDs into `out`
 * (capacity `max`).  Numeric-only tokens are required (guards against accidental
 * fuzzy-text operations); non-numeric / non-positive tokens are counted in
 * *invalid_out and skipped.  *capped_out is set when more than `max` IDs were
 * present.  Returns the number of IDs written.  Shared by 'forget' and 'get'. */
int memory_parse_id_list(const char *s, int64_t *out, int max, int *invalid_out, bool *capped_out) {
   int id_count = 0;
   int invalid_count = 0;
   bool capped = false;

   while (s && *s) {
      while (*s == ',' || *s == ' ' || *s == '\t')
         s++;
      if (!*s)
         break;
      if (id_count >= max) {
         capped = true;
         break;
      }
      char *endptr = NULL;
      errno = 0;
      long long v = strtoll(s, &endptr, 10);
      const char *after = endptr;
      while (*after == ' ' || *after == '\t')
         after++;
      if (endptr == s || (*after != '\0' && *after != ',') || v <= 0 || errno == ERANGE) {
         invalid_count++;
         while (*s && *s != ',') /* skip the bad token */
            s++;
         continue;
      }
      out[id_count++] = (int64_t)v;
      s = after;
   }

   if (invalid_out)
      *invalid_out = invalid_count;
   if (capped_out)
      *capped_out = capped;
   return id_count;
}

/* Sizes of a forget's reply: the first fact's text (shown up to
 * FORGET_FIRST_TEXT_SHOW characters), the id lists, the one-fact and batch
 * replies. */
enum {
   FORGET_FIRST_TEXT_SZ = 256,
   FORGET_FIRST_TEXT_SHOW = 200,
   FORGET_IDS_SZ = 400,
   FORGET_ONE_MSG_SZ = 384,
   FORGET_MSG_SZ = 1600,
};

/* Room an id list keeps for one more ", <id>" (an int64 is 20 digits). */
#define FORGET_ID_ROOM 24

/* What a forget call did, id by id. */
typedef struct {
   int done;    /* forgotten, merged or filed */
   int missing; /* no such fact of the user's */
   int already; /* already merged or replaced (a merge or replace refused) */
   char first_text[FORGET_FIRST_TEXT_SZ];
   bool first_cut;
   char done_ids[FORGET_IDS_SZ];
   char missing_ids[FORGET_IDS_SZ];
   char already_ids[FORGET_IDS_SZ];
   int64_t back_ids[FORGET_BACK_MAX]; /* the values brought back */
   int back_count;
} forget_tally_t;

static void id_list_add(char *list, int64_t id) {
   const size_t len = strlen(list);
   if (len + FORGET_ID_ROOM < FORGET_IDS_SZ) {
      snprintf(list + len, FORGET_IDS_SZ - len, "%s%lld", len ? ", " : "", (long long)id);
   }
}

/* Count what forgetting, merging or filing @p id did (@p rc; @p fact NULL
 * when it wasn't the user's) in @p t. */
static void forget_tally(forget_tally_t *t,
                         int64_t id,
                         const memory_fact_t *fact,
                         int rc,
                         int64_t keeper_id,
                         const memory_forget_result_t *fr) {
   if (rc != MEMORY_DB_SUCCESS) {
      /* A merge or replace refuses a fact already superseded: say which, so
       * the model doesn't think it's gone. */
      const bool already = fact && keeper_id > 0 && fact->superseded_by != 0;
      t->missing += !already;
      t->already += already;
      id_list_add(already ? t->already_ids : t->missing_ids, id);
      return;
   }
   if (t->done == 0) {
      snprintf(t->first_text, sizeof(t->first_text), "%.*s", FORGET_FIRST_TEXT_SHOW,
               fact->fact_text);
      t->first_cut = strlen(fact->fact_text) > FORGET_FIRST_TEXT_SHOW;
   }
   t->done++;
   id_list_add(t->done_ids, id);
   for (int k = 0; k < fr->restored && k < MEMORY_FORGET_RESTORED_MAX; k++) {
      if (t->back_count < FORGET_BACK_MAX) {
         t->back_ids[t->back_count++] = fr->restored_ids[k];
      }
   }
}

/* The values brought back, named if still current (a later id in the call
 * may have forgotten one). */
static void forget_back_text(int user_id, const forget_tally_t *t, strbuf_t *back) {
   for (int k = 0; k < t->back_count; k++) {
      memory_fact_t f;
      if (memory_db_fact_get(t->back_ids[k], user_id, &f) == MEMORY_DB_SUCCESS &&
          f.superseded_by == 0) {
         strbuf_appendf(back, "%s[%lld] \"%.120s\"", back->len ? "; " : "", (long long)f.id,
                        f.fact_text);
      }
   }
}

/* The reply for one id that worked. */
static char *forget_one_reply(const forget_tally_t *t, int64_t id, int64_t keeper_id, bool newer) {
   char *msg = malloc(FORGET_ONE_MSG_SZ);
   if (!msg) {
      return strdup(keeper_id > 0 ? "Fact merged successfully." : "Fact forgotten successfully.");
   }
   const char *cut = t->first_cut ? "..." : "";
   if (keeper_id > 0 && newer) {
      snprintf(msg, FORGET_ONE_MSG_SZ,
               "ID %lld is now an older value ID %lld replaced (hidden from recall; back if "
               "%lld is forgotten): \"%s%s\"",
               (long long)id, (long long)keeper_id, (long long)keeper_id, t->first_text, cut);
   } else if (keeper_id > 0) {
      snprintf(msg, FORGET_ONE_MSG_SZ,
               "Merged ID %lld into ID %lld (hidden from recall, recoverable): \"%s%s\"",
               (long long)id, (long long)keeper_id, t->first_text, cut);
   } else {
      snprintf(msg, FORGET_ONE_MSG_SZ, "Forgotten (ID %lld): \"%s%s\"", (long long)id,
               t->first_text, cut);
   }
   return msg;
}

/* The reply for several ids, or one that didn't work. */
static char *forget_batch_reply(const forget_tally_t *t,
                                int64_t keeper_id,
                                bool newer,
                                int invalid_count,
                                bool capped) {
   strbuf_t out;
   strbuf_init(&out, FORGET_MSG_SZ);
   if (keeper_id > 0) {
      strbuf_appendf(&out, "%s %d fact%s %s ID %lld", newer ? "Filed" : "Merged", t->done,
                     t->done == 1 ? "" : "s", newer ? "as older values replaced by" : "into",
                     (long long)keeper_id);
   } else {
      strbuf_appendf(&out, "Forgotten %d fact%s", t->done, t->done == 1 ? "" : "s");
   }
   if (t->done > 0) {
      strbuf_appendf(&out,
                     keeper_id > 0 ? " (IDs %s; hidden from recall, recoverable)" : " (IDs %s)",
                     t->done_ids);
   }
   if (t->already > 0) {
      strbuf_appendf(&out, "; %d already merged or replaced, so left as they are (%s)", t->already,
                     t->already_ids);
   }
   if (t->missing > 0) {
      strbuf_appendf(&out, "; %d not found (%s)", t->missing, t->missing_ids);
   }
   if (invalid_count > 0) {
      strbuf_appendf(&out, "; ignored %d non-numeric token%s", invalid_count,
                     invalid_count == 1 ? "" : "s");
   }
   if (capped) {
      strbuf_appendf(&out, " (capped at %d IDs per call)", MEMORY_FORGET_IDS_MAX);
   }
   char *msg = strbuf_steal(&out);
   return msg ? msg : strdup("Memory updated.");
}

/* Why the kept fact can't keep @p ids, or NULL when it can: it must be the
 * user's, current, and not one of them. */
static char *keeper_refusal(int user_id, int64_t keeper_id, const int64_t *ids, int n) {
   memory_fact_t keep;
   if (memory_db_fact_get(keeper_id, user_id, &keep) != MEMORY_DB_SUCCESS) {
      return strdup(TOOL_RESULT_ERROR_MARK
                    "The kept fact's ID wasn't found (or isn't yours). Nothing was changed.");
   }
   if (keep.superseded_by != 0) {
      return strdup(TOOL_RESULT_ERROR_MARK "The kept fact was itself merged or replaced; keep the "
                                           "current one instead. Nothing was changed.");
   }
   for (int i = 0; i < n; i++) {
      if (ids[i] == keeper_id) {
         return strdup("The kept fact can't be one of the IDs being forgotten. Nothing was "
                       "changed.");
      }
   }
   return NULL;
}

/* 'forget' with @p keeper_id 0 deletes; with a kept fact's ID it merges the
 * IDs into it as duplicates, or with @p newer files them as older values that
 * fact replaced. */
static char *forget_ids(int user_id, const char *fact_text, int64_t keeper_id, bool newer) {
   int64_t ids[MEMORY_FORGET_IDS_MAX];
   int invalid_count = 0;
   bool capped = false;
   const int n = fact_text ? memory_parse_id_list(fact_text, ids, MEMORY_FORGET_IDS_MAX,
                                                  &invalid_count, &capped)
                           : 0;
   if (n == 0) {
      return strdup("Please provide numeric fact ID(s) (e.g. '42' or '42,57,91'). Use 'search', "
                    "'recent', or 'find_duplicates' first to find the IDs.");
   }
   if (keeper_id > 0) {
      char *refused = keeper_refusal(user_id, keeper_id, ids, n);
      if (refused) {
         return refused;
      }
   }
   /* A delete waits once, for the whole batch, for the user's running
    * extraction (one finishing meanwhile could merge into a fact being
    * forgotten), and is the user removing it (a merge isn't: the kept fact
    * says it). */
   const bool del = keeper_id <= 0;
   if (del && !memory_extraction_hold_user(user_id, FORGET_TOOL_WAIT_SEC)) {
      return strdup(TOOL_RESULT_ERROR_MARK "Memory is still being saved; nothing was forgotten. "
                                           "Try again in a moment.");
   }
   if (del) {
      conv_db_withdraw_intent_begin(user_id);
   }
   forget_tally_t t;
   memset(&t, 0, sizeof(t));
   for (int i = 0; i < n; i++) {
      memory_fact_t fact;
      memory_forget_result_t fr = { 0 };
      const bool mine = memory_db_fact_get(ids[i], user_id, &fact) == MEMORY_DB_SUCCESS;
      const int rc = !mine   ? MEMORY_DB_NOT_FOUND
                     : del   ? memory_db_fact_forget(ids[i], user_id, &fr)
                     : newer ? memory_db_fact_replace(ids[i], keeper_id, user_id)
                             : memory_db_fact_merge(ids[i], keeper_id, user_id);
      forget_tally(&t, ids[i], mine ? &fact : NULL, rc, keeper_id, &fr);
   }
   if (del) {
      conv_db_withdraw_intent_end();
      memory_extraction_release_user(user_id);
   }
   strbuf_t back;
   strbuf_init(&back, 256);
   forget_back_text(user_id, &t, &back);
   /* What was forgotten leaves the conversations it was sent into. */
   if (del && t.done > 0) {
      session_withdraw_forgotten_async(user_id, false);
   }
   char *msg = (n == 1 && t.done == 1)
                   ? forget_one_reply(&t, ids[0], keeper_id, newer)
                   : forget_batch_reply(&t, keeper_id, newer, invalid_count, capped);
   return forget_reply(msg, &back);
}

/* @p name's value from @p value: a positive id, -1 when malformed (refused,
 * not read as "none"), or 0 when absent, empty or 0 (a model filling an
 * optional number). */
static int64_t keeper_param(const char *value, const char *name) {
   char text[32] = "";
   if (!tool_param_extract_custom(value, name, text, sizeof(text)) || !text[0] ||
       strcmp(text, "0") == 0) {
      return 0;
   }
   char *end = NULL;
   errno = 0;
   const long long id = strtoll(text, &end, 10);
   while (end && (*end == ' ' || *end == '\t')) {
      end++;
   }
   return (errno != 0 || end == text || !end || *end != '\0' || id <= 0) ? -1 : (int64_t)id;
}

char *memory_action_forget(int user_id, const char *value) {
   /* IDs are the base value.  duplicate_of merges them into a kept fact;
    * newer_fact_id files them as older values a newer fact replaced.
    * replaced_by is what duplicate_of was called before: conversations whose
    * tool definition predates the rename still send it, so it merges. */
   char ids[1024] = "";
   tool_param_extract_base(value, ids, sizeof(ids));
   const int64_t dup = keeper_param(value, "duplicate_of");
   const int64_t legacy = keeper_param(value, "replaced_by");
   const int64_t newer = keeper_param(value, "newer_fact_id");
   if (dup < 0 || legacy < 0 || newer < 0) {
      return strdup(TOOL_RESULT_ERROR_MARK "The kept fact's ID must be a single positive number. "
                                           "Nothing was changed.");
   }
   if ((dup > 0) + (legacy > 0) + (newer > 0) > 1) {
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Set 'duplicate_of' (same information) or 'newer_fact_id' (a newer value), "
                    "not both. Nothing was changed.");
   }
   if (newer > 0) {
      return forget_ids(user_id, ids, newer, true);
   }
   return forget_ids(user_id, ids, dup > 0 ? dup : legacy, false);
}
