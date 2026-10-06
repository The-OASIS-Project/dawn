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
 * A turn's retrieved items at its seam (session_focus.h).
 */

#include "core/session_focus.h"

#include <json-c/json.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/focus/focus_handles.h"
#include "core/session_manager.h"
#include "llm/llm_history_kind.h"
#include "logging.h"
#include "utils/string_utils.h"

__attribute__((weak)) void session_focus_client_notice(session_t *session,
                                                       const composed_prompt_t *cp,
                                                       const focus_item_state_t *states,
                                                       int n_states) {
   (void)session;
   (void)cp;
   (void)states;
   (void)n_states;
}

/* The context an earlier attempt of @p envelope put just before it, which
 * this apply replaces; NULL when there is none. */
static struct json_object *envelope_attempt(struct json_object *hist,
                                            struct json_object *envelope) {
   const int len = (int)json_object_array_length(hist);
   for (int i = len - 1; i > 0; i--) {
      if (json_object_array_get_idx(hist, i) == envelope) {
         struct json_object *before = json_object_array_get_idx(hist, i - 1);
         return llm_history_is_unsaved_context(before) ? before : NULL;
      }
   }
   return NULL;
}

/* Caller holds history_mutex.  Whether the session's handle table numbers
 * @p hist_conv's items: the conversation's own; or, while the history has no
 * conversation yet, a table of none, or of the conversation the turn runs
 * for (the table is adopted when the history is saved as it). */
static bool table_is_hists_locked(const session_t *session, int64_t hist_conv) {
   const focus_handles_t *t = session->focus_handles;
   if (!t) {
      return false;
   }
   if (t->conv_id == hist_conv) {
      return true;
   }
   const int64_t turn_conv = session->turn_active ? session->turn_history_conv
                                                  : atomic_load(&session->history_conversation_id);
   return hist_conv == 0 && (t->conv_id == 0 || t->conv_id == turn_conv);
}

static bool in_ids(struct json_object *ids, const char *id) {
   const size_t n = (ids && id && *id) ? json_object_array_length(ids) : 0;
   for (size_t i = 0; i < n; i++) {
      const char *w = json_object_get_string(json_object_array_get_idx(ids, i));
      if (w && strcmp(w, id) == 0) {
         return true;
      }
   }
   return false;
}

static int cmp_int(const void *a, const void *b) {
   const int x = *(const int *)a;
   const int y = *(const int *)b;
   return (x > y) - (x < y);
}

/* The handles @p scan shows (not withdrawn), sorted, into @p out. */
static void keep_visible(const focus_scan_t *scan, session_focus_turn_t *out) {
   out->visible = scan->count ? malloc((size_t)scan->count * sizeof(*out->visible)) : NULL;
   for (int i = 0; out->visible && i < scan->count; i++) {
      if (!scan->items[i].withdrawn) {
         out->visible[out->n_visible++] = scan->items[i].handle;
      }
   }
   if (out->n_visible > 1) {
      qsort(out->visible, (size_t)out->n_visible, sizeof(*out->visible), cmp_int);
   }
}

/* The turn's items as this seam sends them (see session_focus_turn_t). */
static bool choose_items(const composed_prompt_t *cp,
                         struct json_object *withdrawn_ids,
                         session_focus_turn_t *out) {
   const int n = cp->n_focus_items;
   out->items = calloc((size_t)n, sizeof(*out->items));
   out->from = calloc((size_t)n, sizeof(*out->from));
   if (!out->items || !out->from) {
      return false;
   }
   for (int j = 0; j < n; j++) {
      const prompt_focus_item_t *it = &cp->focus_items[j];
      if (in_ids(withdrawn_ids, it->item_id)) {
         continue; /* forgotten since it was retrieved */
      }
      out->items[out->n] = *it;
      if (!out->numbered) {
         out->items[out->n].handle = 0;
      }
      out->from[out->n++] = j;
   }
   return true;
}

char *session_focus_items_locked(session_t *session,
                                 struct json_object *hist,
                                 int64_t hist_conv,
                                 struct json_object *question,
                                 const char *tag,
                                 const composed_prompt_t *cp,
                                 struct json_object *withdrawn_ids,
                                 session_focus_turn_t *out) {
   if (!out) {
      return NULL;
   }
   memset(out, 0, sizeof(*out));
   const bool citation_on = g_config.memory.citation_enabled;
   const int n = cp ? cp->n_focus_items : 0;
   if (!session || !hist || (n <= 0 && !citation_on)) {
      return NULL;
   }
   out->numbered = table_is_hists_locked(session, hist_conv);
   struct json_object *skip[2] = { question, NULL };
   int n_skip = question ? 1 : 0;
   if (question && llm_history_kind_of(question) == MESSAGE_KIND_ENVELOPE) {
      skip[1] = envelope_attempt(hist, question);
      n_skip += skip[1] != NULL;
   }
   /* A history with no tag isn't frozen (its prefix couldn't be made): its
    * contexts can't be told from imitations, so it shows nothing and every
    * item is sent. */
   focus_scan_t scan = { 0 };
   if (tag && focus_incremental_scan(hist, tag, skip, n_skip, &scan) != 0) {
      /* What wasn't read counts as not shown: its items are sent again. */
      OLOG_WARNING("Session %u: out of memory reading the items its history shows",
                   session->session_id);
   }
   keep_visible(&scan, out);
   char *text = NULL;
   if (n > 0) {
      if (!choose_items(cp, withdrawn_ids, out) ||
          focus_incremental_select(out->items, out->n, tag, &scan, &out->sel) != 0) {
         OLOG_ERROR("Session %u: out of memory choosing the turn's items; none sent",
                    session->session_id);
      } else {
         text = focus_incremental_render(out->items, &out->sel, citation_on);
      }
   }
   /* The scan borrows the history's text, which the context about to go in
    * may change: only the handles it showed are kept past here. */
   focus_scan_free(&scan);
   return text;
}

/* Caller holds history_mutex.  This turn's citation map: the items it sent
 * (in render order), then those it named again. */
static void stash_locked(session_t *session, const session_focus_turn_t *t) {
   citation_stash_t *stash = &session->citation.stash;
   memset(stash, 0, sizeof(*stash));
   for (int pass = 0; pass < 2; pass++) {
      for (int i = 0; i < t->sel.n && stash->count < MAX_CITATION_STASH; i++) {
         const bool referenced = t->sel.states[i] == FOCUS_ITEM_REFERENCED;
         const bool take = pass == 0 ? t->sel.rendered[i] : referenced;
         if (!take || t->items[i].handle <= 0 || t->items[i].item_id[0] == '\0') {
            continue;
         }
         citation_stash_entry_t *e = &stash->entries[stash->count++];
         e->handle = t->items[i].handle;
         safe_strscpy(e->item_id, t->items[i].item_id);
         e->final_score = t->items[i].score;
         e->referenced = referenced;
      }
   }
}

/* Caller holds history_mutex.  The conversation's earlier items the history
 * still shows (citable from an earlier turn), at most one per handle it
 * shows.  This turn's own items are in its stash. */
static void prior_locked(session_t *session, const session_focus_turn_t *t) {
   free(session->citation.prior);
   session->citation.prior = NULL;
   session->citation.prior_count = 0;
   const focus_handles_t *table = session->focus_handles;
   if (!t->numbered || !table || table->count == 0 || t->n_visible == 0) {
      return;
   }
   session->citation.prior = malloc((size_t)t->n_visible * sizeof(*session->citation.prior));
   for (int k = 0;
        session->citation.prior && k < table->count && session->citation.prior_count < t->n_visible;
        k++) {
      const int h = table->items[k].handle;
      if (bsearch(&h, t->visible, (size_t)t->n_visible, sizeof(int), cmp_int)) {
         citation_prior_t *p = &session->citation.prior[session->citation.prior_count++];
         p->handle = h;
         safe_strscpy(p->item_id, table->items[k].item_id);
      }
   }
}

void session_focus_commit_locked(session_t *session, session_focus_turn_t *turn, bool attached) {
   if (!session || !turn) {
      return;
   }
   if (!attached) {
      focus_selection_not_attached(&turn->sel);
   }
   if (turn->sel.n > 0) {
      OLOG_INFO("focus: session %u: %d relevant item(s): %d sent, %d already shown, %d left out",
                session->session_id, turn->sel.n, turn->sel.n_rendered,
                turn->sel.n - turn->sel.n_sent, turn->sel.n_sent - turn->sel.n_rendered);
   }
   if (g_config.memory.citation_enabled) {
      if (attached) {
         stash_locked(session, turn);
      }
      prior_locked(session, turn);
   }
}

void session_focus_notify(session_t *session,
                          const composed_prompt_t *cp,
                          session_focus_turn_t *turn) {
   if (session && cp && cp->focus_panel) {
      /* Each of the prompt's items: its place, or left out (forgotten since
       * it was retrieved, never chosen, or its context never went in). */
      focus_item_state_t *states = NULL;
      if (cp->n_focus_items > 0) {
         states = malloc((size_t)cp->n_focus_items * sizeof(*states));
         for (int j = 0; states && j < cp->n_focus_items; j++) {
            states[j] = FOCUS_ITEM_LEFT_OUT;
         }
         for (int i = 0; states && turn && turn->from && turn->sel.states && i < turn->sel.n; i++) {
            states[turn->from[i]] = turn->sel.states[i];
         }
      }
      session_focus_client_notice(session, cp, states, states ? cp->n_focus_items : 0);
      free(states);
   }
   if (turn) {
      focus_selection_free(&turn->sel);
      free(turn->items);
      free(turn->from);
      free(turn->visible);
      memset(turn, 0, sizeof(*turn));
   }
}

void session_citation_stash_clear(session_t *session) {
   if (session == NULL) {
      return;
   }
   pthread_mutex_lock(&session->history_mutex);
   /* All of what the last turn's reply could cite; which earlier items are
    * citable is the next seam's to say. */
   free(session->citation.prior);
   memset(&session->citation, 0, sizeof(session->citation));
   pthread_mutex_unlock(&session->history_mutex);
}
