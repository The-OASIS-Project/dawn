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
 * Per-turn focus builder: the items retrieved for a turn's context, ranked,
 * each numbered for the conversation, and what the client's context panel is
 * told about them.  See header for the full contract.
 */

#include "core/build_focus_block.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/focus/focus_candidate_helpers.h"
#include "core/focus/focus_handles.h"
#include "core/focus/focus_incremental.h"
#include "core/focus/focus_source.h"
#include "core/session_focus.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "llm/llm_context_text.h"
#include "logging.h"
#include "memory/memory_embeddings.h"
#include "utils/string_utils.h"
#include "webui/webui_server.h"

/* Truncate user_turn_text to this many chars when building the
 * privacy-safe LOG_INFO summary.  Logs go to syslog; the daemon does
 * not log raw fact / message text by convention. */
#define FOCUS_LOG_TURN_EXCERPT_CHARS 32

static double monotonic_ms_now(void) {
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return 0.0;
   return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

/* Privacy-safe per-call log line — excerpts at most
 * FOCUS_LOG_TURN_EXCERPT_CHARS of the user query and NEVER the
 * candidate text content. */
static void log_focus_summary(const char *turn_text,
                              int candidates,
                              int rejections,
                              double elapsed_ms) {
   char excerpt[FOCUS_LOG_TURN_EXCERPT_CHARS + 1];
   const size_t turn_len = (turn_text != NULL) ? strlen(turn_text) : 0;
   const size_t copy = (turn_len > FOCUS_LOG_TURN_EXCERPT_CHARS) ? FOCUS_LOG_TURN_EXCERPT_CHARS
                                                                 : turn_len;
   if (copy > 0)
      memcpy(excerpt, turn_text, copy);
   excerpt[copy] = '\0';
   OLOG_INFO("focus: turn=\"%s%s\" candidates=%d rejections=%d elapsed_ms=%.1f", excerpt,
             turn_len > FOCUS_LOG_TURN_EXCERPT_CHARS ? "..." : "", candidates, rejections,
             elapsed_ms);
}

/* What the context panel is shown of a turn's retrieval: the ranked result,
 * its candidates the ones that became the prompt's items (same order), and
 * where it was asked from.  Owned by the composed prompt until its seam has
 * decided each item's place (session_focus_client_notice). */
typedef struct focus_panel {
   focus_compose_result_t result;
   int user_id;
   int64_t conv_id;
   int64_t turn_id;
} focus_panel_t;

static void focus_panel_free(focus_panel_t *panel) {
   if (panel) {
      focus_result_free(&panel->result);
      free(panel);
   }
}

/* Drop candidate @p i from @p r's working list (its heap freed). */
static void drop_candidate(focus_compose_result_t *r, int i) {
   free(r->candidates[i].text);
   free(r->candidates[i].item_id);
   r->candidates[i].text = NULL;
   r->candidates[i].item_id = NULL;
}

/* A question this short ("what's that for?", "and the other one?") leans on
 * the one before it, so retrieval's embedding reads both. */
#define FOCUS_FOLLOWUP_MAX_WORDS 6
/* The earlier question's bytes, at most, that go with it. */
#define FOCUS_FOLLOWUP_PREV_MAX_BYTES 600

static int word_count(const char *text) {
   int words = 0;
   bool in_word = false;
   for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
      const bool space = *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r';
      words += !space && !in_word;
      in_word = !space;
   }
   return words;
}

/* The text retrieval embeds: a short follow-up with the previous question in
 * front of it (heap, caller frees), else NULL (embed the turn's own text).
 * Only the embedding reads it: keyword and date matching stay on the turn's
 * own words, so a "tomorrow?" isn't read as the "today" asked about before. */
static char *followup_embed_text(session_t *session, const char *turn_text) {
   if (!session || word_count(turn_text) > FOCUS_FOLLOWUP_MAX_WORDS) {
      return NULL;
   }
   char *prev = session_previous_question_dup(session);
   if (!prev) {
      return NULL;
   }
   const size_t prev_len = focus_utf8_safe_cap(prev, FOCUS_FOLLOWUP_PREV_MAX_BYTES);
   const size_t turn_len = strlen(turn_text);
   char *text = malloc(prev_len + 1 + turn_len + 1);
   if (text) {
      memcpy(text, prev, prev_len);
      text[prev_len] = '\n';
      memcpy(text + prev_len + 1, turn_text, turn_len + 1);
   }
   free(prev);
   return text;
}

/* " YYYY-MM-DD" (local time) for an item's timestamp, or "" without one: the
 * model can tell an old item from a current one (see the context rules). */
static void item_date(time_t ts, char *buf, size_t size) {
   struct tm tm_storage;
   buf[0] = '\0';
   if (ts > 0 && localtime_r(&ts, &tm_storage) != NULL &&
       strftime(buf, size, " %Y-%m-%d", &tm_storage) == 0) {
      buf[0] = '\0';
   }
}

int build_focus_block(session_t *session,
                      int user_id,
                      int64_t conv_id,
                      int64_t turn_id,
                      const char *user_turn_text,
                      composed_prompt_t *out) {
   if (out == NULL)
      return FAILURE;

   /* Snapshot the focus_injection sub-config under one read — guards
    * against TOCTOU when the WebUI settings handler mutates `enabled`
    * + `top_k` from a different thread mid-call. */
   const focus_injection_config_t fi = g_config.memory.focus_injection;

   /* Gate 1: feature flag.  ZERO embedding compute, ZERO focus_compose
    * call when disabled. */
   if (!fi.enabled)
      return SUCCESS;

   /* Gate 2: caller contract.  Unauthenticated callers and empty
    * turn text short-circuit silently. */
   if (user_id <= 0 || user_turn_text == NULL || user_turn_text[0] == '\0')
      return SUCCESS;

   const double t_start = monotonic_ms_now();

   /* Embed the user turn.  On failure, fall through to focus_compose
    * with NULL embedding — adapters with requires_embedding=true skip
    * themselves; calendar (requires_embedding=false) still consults.
    * The embedding must outlive focus_compose (adapters read it
    * synchronously), so it stays at function scope. */
   float query_embed[MAX_EMBEDDING_DIMS];
   int embed_dims = 0;
   const float *query_ptr = NULL;
   if (memory_embeddings_available()) {
      char *followup = followup_embed_text(session, user_turn_text);
      if (followup) {
         OLOG_INFO("focus: short follow-up; its embedding reads the previous question too");
      }
      const int embed_rc = memory_embeddings_embed(followup ? followup : user_turn_text,
                                                   query_embed, &embed_dims);
      free(followup);
      if (embed_rc == SUCCESS && embed_dims > 0) {
         query_ptr = query_embed;
      } else {
         OLOG_WARNING("focus: embedding compute failed for user_id=%d — "
                      "passing NULL embedding to focus_compose (vector adapters skip)",
                      user_id);
         embed_dims = 0;
      }
   }
   /* A backend that returned a negative dim would wrap to a huge size_t. */
   if (embed_dims < 0)
      embed_dims = 0;

   /* top_k is relevance: the top_k most relevant, with no over-fetch.  An
    * item the conversation already shows is named at the seam, not
    * replaced by a less relevant one. */
   const time_t now = time(NULL);
   const int top_k = fi.top_k > 0 ? fi.top_k : 8;

   focus_panel_t *panel = calloc(1, sizeof(*panel));
   if (panel == NULL) {
      OLOG_ERROR("focus: out of memory for the turn's retrieval");
      return FAILURE;
   }
   focus_compose_result_t *result = &panel->result;
   const int rc = focus_compose(user_id, /*include_private*/ false, user_turn_text, query_ptr,
                                (size_t)embed_dims, now, top_k, result);
   if (rc != SUCCESS) {
      OLOG_WARNING("focus: focus_compose failed (user_id=%d)", user_id);
      focus_panel_free(panel);
      return FAILURE;
   }

   const bool citation_on = g_config.memory.citation_enabled;
   const int n_cand = result->candidate_count > 0 ? result->candidate_count : 0;

   /* Each item's handle: stable for the conversation's life when a session
    * holds it ([M7] is the same item on every turn and reload, and the
    * conversation's record of what it was sent: a forgotten item's copy is
    * withdrawn by it), else this turn's own numbering (citation only). */
   int handles[MAX_CITATION_STASH] = { 0 };
   if (session != NULL) {
      conv_focus_handle_t want[MAX_CITATION_STASH];
      int want_idx[MAX_CITATION_STASH];
      int n_want = 0;
      for (int i = 0; i < n_cand && n_want < MAX_CITATION_STASH; i++) {
         const focus_candidate_t *c = &result->candidates[i];
         if (c->text != NULL && c->text[0] != '\0' && c->item_id != NULL && c->source_id != NULL) {
            want[n_want] = (conv_focus_handle_t){ .source = c->source_id, .item_id = c->item_id };
            want_idx[n_want++] = i;
         }
      }
      const int64_t live_conv = session_turn_conversation(session);
      (void)focus_handles_assign(session, live_conv > 0 ? live_conv : conv_id, user_id, want,
                                 n_want);
      for (int k = 0; k < n_want; k++) {
         handles[want_idx[k]] = want[k].handle;
      }
   }

   prompt_focus_item_t *items = n_cand > 0 ? calloc((size_t)n_cand, sizeof(*items)) : NULL;
   if (n_cand > 0 && items == NULL) {
      OLOG_ERROR("focus: out of memory for the turn's items");
      focus_panel_free(panel);
      return FAILURE;
   }
   int kept = 0;
   int ordinal = 0;
   for (int i = 0; i < n_cand; i++) {
      focus_candidate_t *c = &result->candidates[i];
      const bool has_text = c->text != NULL && c->text[0] != '\0' && c->source_id != NULL;
      int handle = 0;
      if (session != NULL) {
         handle = i < MAX_CITATION_STASH ? handles[i] : 0;
      } else if (citation_on && c->item_id != NULL && ordinal < MAX_CITATION_STASH) {
         handle = ++ordinal;
      }
      /* A held item with no number couldn't be withdrawn if it were
       * forgotten: left out (only past the handle cap). */
      const bool unnumbered_held = session != NULL && c->item_id != NULL && handle <= 0;
      /* An item's text can come from anywhere (a calendar invite, a document,
       * a fact learned from a web page): DAWN's markers in it are defused, and
       * it is one line (its line is what a withdrawal replaces).  The seam
       * masks it with the conversation's tag. */
      char *text = (has_text && !unnumbered_held) ? llm_context_neutralize_line(c->text) : NULL;
      if (has_text && !unnumbered_held && text == NULL) {
         OLOG_ERROR("focus: out of memory preparing candidate %d; left out", i);
      }
      if (text == NULL) {
         drop_candidate(result, i);
         continue;
      }
      prompt_focus_item_t *it = &items[kept];
      it->handle = handle;
      safe_strscpy(it->source, c->source_id);
      if (c->item_id != NULL) {
         safe_strscpy(it->item_id, c->item_id);
      }
      item_date(c->item_timestamp, it->date, sizeof(it->date));
      it->text = text;
      it->score = result->score_breakdowns != NULL ? result->score_breakdowns[i].final_score
                                                   : FOCUS_SCORE_NA;
      /* The panel's rows are the items, in the same order. */
      if (i != kept) {
         result->candidates[kept] = *c;
         if (result->score_breakdowns != NULL) {
            result->score_breakdowns[kept] = result->score_breakdowns[i];
         }
         c->text = NULL;
         c->item_id = NULL;
      }
      kept++;
   }
   result->candidate_count = kept;

   /* Per-source rejection logging — fires only when the framework
    * filter caught poison in adapter output. */
   for (int i = 0; i < result->rejection_count; i++) {
      const focus_filter_rejection_t *r = &result->rejections[i];
      if (r->count > 0)
         OLOG_WARNING("focus: filter rejected %d candidate(s) from source='%s' (user_id=%d)",
                      r->count, r->source_id, user_id);
   }

   panel->user_id = user_id;
   panel->conv_id = conv_id;
   panel->turn_id = turn_id;
   out->focus_items = items;
   out->n_focus_items = kept;
   if (kept == 0) {
      free(items);
      out->focus_items = NULL;
   }
   out->focus_panel = panel;
   out->focus_panel_free = focus_panel_free;

   log_focus_summary(user_turn_text, kept, result->rejection_count, monotonic_ms_now() - t_start);
   return SUCCESS;
}

/* The context panel: every item the retrieval found, each with its place in
 * the turn (sent, or already in context), told to the browser tabs showing
 * the conversation.  Replaces the session unit's no-op; runs after the seam
 * released the history lock.  An empty retrieval is told too, so the panel
 * shows "looked, found nothing".
 *
 * The conversation is re-read from the session: on a new chat's first turn
 * the client names its conversation just after sending the question, so the
 * one captured when the prompt was built can still be 0 (seen live, ~47 ms
 * apart); by the seam it is set. */
void session_focus_client_notice(session_t *session,
                                 const composed_prompt_t *cp,
                                 const focus_item_state_t *states,
                                 int n_states) {
   if (cp == NULL || cp->focus_panel == NULL || cp->focus_panel_free != focus_panel_free) {
      return;
   }
   const focus_panel_t *panel = cp->focus_panel;
   int64_t conv = panel->conv_id;
   if (session != NULL) {
      const int64_t live = session_turn_conversation(session);
      if (live > 0) {
         conv = live;
      }
   }
   if (conv <= 0) {
      return;
   }
   const int n = panel->result.candidate_count;
   if (states && n_states != n) {
      OLOG_WARNING("focus: %d state(s) for %d panel item(s); showing every item as new", n_states,
                   n);
      states = NULL;
   }
   const char **names = n > 0 ? calloc((size_t)n, sizeof(*names)) : NULL;
   for (int i = 0; names && i < n; i++) {
      names[i] = focus_item_state_name(states ? states[i] : FOCUS_ITEM_NEW);
   }
   webui_broadcast_context_injection(panel->user_id, conv, panel->turn_id, &panel->result, names);
   free(names);
}
