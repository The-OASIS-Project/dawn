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
 * Per-turn focus-block builder.  See header for the full contract.
 * Prepends a synthetic `[system_time]` candidate when a real dispatch
 * session is published; replaces the retired prompt_compose_build_now_block
 * surface from before the prompt-cache split.
 */

#include "webui/build_focus_block.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/focus/focus_source.h"
#include "core/session_manager.h"
#include "core/strbuf.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_embeddings.h"
#include "webui/webui_server.h"

/* Per-call rendering buffer initial size — sized to comfortably hold
 * the typical ~8-candidate result.  Grows via strbuf if exceeded.
 *
 * Max cap: top_k_max (64) × (FOCUS_TEXT_MAX_BYTES (4096) + framing
 * overhead per line ~96) + slack ≈ 270 KB worst case.  Practical
 * top_k default (8) × 4 KB = ~33 KB lands right at the previous 32 KB
 * cap and silently truncated under document-heavy turns; sized to
 * 64 KB to comfortably hold the typical 8-candidate document-heavy
 * case while still bounded against runaway adapter output. */
#define FOCUS_BLOCK_INIT_BYTES 4096
#define FOCUS_BLOCK_MAX_BYTES (64 * 1024)

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
 * candidate text content.  Extracted from inline duplicate code at
 * the empty-result and success paths so the format string is one
 * source of truth.
 *
 * 1f addition: `dedup_suppressed` is a grep-friendly counter for the
 * number of focus_compose candidates that the per-session dedup state
 * dropped this turn.  Always emitted (zero when dedup didn't run). */
static void log_focus_summary(const char *turn_text,
                              int candidates,
                              int rejections,
                              int dedup_suppressed,
                              double elapsed_ms) {
   char excerpt[FOCUS_LOG_TURN_EXCERPT_CHARS + 1];
   const size_t turn_len = (turn_text != NULL) ? strlen(turn_text) : 0;
   const size_t copy = (turn_len > FOCUS_LOG_TURN_EXCERPT_CHARS) ? FOCUS_LOG_TURN_EXCERPT_CHARS
                                                                 : turn_len;
   if (copy > 0)
      memcpy(excerpt, turn_text, copy);
   excerpt[copy] = '\0';
   OLOG_INFO("focus: turn=\"%s%s\" candidates=%d rejections=%d dedup_suppressed=%d elapsed_ms=%.1f",
             excerpt, turn_len > FOCUS_LOG_TURN_EXCERPT_CHARS ? "..." : "", candidates, rejections,
             dedup_suppressed, elapsed_ms);
}

/* Compute the over-fetch hint passed to focus_compose.
 *
 * Phase 1f adds dedup-suppression on top of focus_compose's ranked
 * output.  If we asked for exactly top_k candidates, dedup-suppression
 * would shrink the surfaced block below the user-configured size on
 * busy turns.  Over-fetch by half the dedup window so the typical
 * suppress-everything-old turn still yields ~top_k survivors:
 *
 *     per_source_max = top_k + recent_window/2
 *     per_source_max = MIN(per_source_max, top_k * 2)
 *
 * Capped at 2× top_k so a pathological recent_window setting can't
 * blow latency / token budget.  Default knobs (top_k=8, window=8)
 * land at per_source_max=12 — comfortably under the 2× = 16 cap. */
static int compute_per_source_max(int top_k, int recent_window_turns) {
   if (top_k <= 0)
      top_k = 8;
   const int over_fetch = top_k + (recent_window_turns / 2);
   const int cap = top_k * 2;
   return (over_fetch < cap) ? over_fetch : cap;
}

/* Apply the per-session dedup formula to focus_compose's ranked
 * candidates.  Walks the list under the session's history_mutex, admits
 * up to top_k survivors, and rewrites `result->candidates[]` IN LOCKSTEP
 * with `result->score_breakdowns[]` to the survivor prefix
 * (count = kept).  Suppressed candidates have their text/item_id freed
 * inline so focus_result_free() at the end still cleans up survivors
 * only.  Breakdowns hold no heap pointers — float scalars only — so
 * compaction is a structure copy.
 *
 * `*out_suppressed` receives the count of dropped candidates for the
 * grep-friendly log line.
 *
 * INVARIANT: dedup state ONLY tracks candidates that surfaced to the
 * LLM via this turn's render.  Filter-rejected candidates (memory_filter
 * fail inside focus_compose) never reach this loop, so they never call
 * _record_locked.  If a filter-rejected candidate later survives the
 * filter on a future turn, it admits fresh with no prior history.  Per
 * design doc rev 3 §"State management contracts".
 *
 * Phase 1g-i: score-uplift now reads `result->score_breakdowns[i].final_score`
 * (was `c->semantic_score` — a 1f bug that compared raw embedding
 * cosine instead of the ranker's composed score).  In production with
 * non-trivial weight_recency / weight_importance / weight_source, the
 * two values can disagree on admit/suppress; final_score is what the
 * ranker actually used to order this turn's pool, so it's also what the
 * uplift-compare must beat to mean "noticeably more relevant now." */
static void apply_dedup_locked(session_t *session,
                               focus_compose_result_t *result,
                               int top_k,
                               int recent_window_turns,
                               float min_score,
                               float score_uplift_factor,
                               int *out_suppressed) {
   *out_suppressed = 0;
   if (session == NULL || result == NULL || result->candidates == NULL ||
       result->score_breakdowns == NULL || result->candidate_count <= 0)
      return;

   const int current_turn = session_injected_set_advance_turn_locked(session);

   int kept = 0;
   for (int i = 0; i < result->candidate_count; i++) {
      focus_candidate_t *c = &result->candidates[i];

      bool admit = false;
      injected_set_entry_t prior;
      const char *src = (c->source_id != NULL) ? c->source_id : "";
      const char *iid = (c->item_id != NULL) ? c->item_id : "";

      /* Use the ranker's composed final_score as the candidate's
       * "current relevance" — that's what the rank pass ordered the
       * pool by, and matches what a future call's uplift-compare needs
       * to beat to mean "noticeably more relevant now."  Phase 1f
       * read c->semantic_score here, which is only the raw cosine —
       * weights collapsed it.  See block comment above for context. */
      const float current_score = result->score_breakdowns[i].final_score;

      if (iid[0] == '\0') {
         /* No stable id → cannot dedup.  Admit unconditionally — same
          * behavior as a guaranteed-miss lookup.  This shouldn't happen
          * with shipped adapters (all emit server-generated ids), but
          * the fallback keeps us correct under future adapters that
          * forget. */
         admit = true;
      } else if (session_injected_set_lookup_locked(session, src, iid, &prior) != SUCCESS) {
         /* Never injected before → admit. */
         admit = true;
      } else {
         /* Strict greater-than (NOT >=).  At recent_window_turns=0 any
          * turn after the original admits — score_uplift becomes the
          * only path that can suppress.  Documented in dawn.toml.example.
          *
          * The min_score branch is defense-in-depth — focus_compose
          * already drops sub-min_score candidates before this loop sees
          * them — so it should never trigger today, but keeps the
          * formula correct if focus_compose's drop-below-min behavior
          * changes. */
         const int turns_since = current_turn - prior.last_injected_turn;
         const bool window_passed = (turns_since > recent_window_turns) &&
                                    (current_score >= min_score);
         const bool uplift_met = (current_score >= prior.last_score * score_uplift_factor);
         admit = window_passed || uplift_met;
      }

      if (admit) {
         if (iid[0] != '\0')
            session_injected_set_record_locked(session, src, iid, current_score);
         /* Move into the survivor prefix if not already there.  Both
          * arrays compact in lockstep — when i > kept, ownership of
          * text/item_id moves to slot `kept`, breakdown copies as a
          * plain struct (no heap), and we zero the source-slot heap
          * pointers to prevent the truncation cleanup below from
          * double-freeing.  Breakdown's source-slot is left as-is
          * (cosmetic — past `kept` after compaction so unreachable). */
         if (i != kept) {
            result->candidates[kept] = *c;
            result->score_breakdowns[kept] = result->score_breakdowns[i];
            result->candidates[i].text = NULL;
            result->candidates[i].item_id = NULL;
         }
         kept++;
         if (kept >= top_k) {
            /* Top_k satisfied — drop the rest.  Free heap on candidates
             * from i+1 onward; breakdowns past `kept` need no teardown
             * (no heap pointers).  Over-fetch produced extras we don't
             * render. */
            for (int j = i + 1; j < result->candidate_count; j++) {
               free(result->candidates[j].text);
               free(result->candidates[j].item_id);
               result->candidates[j].text = NULL;
               result->candidates[j].item_id = NULL;
            }
            break;
         }
      } else {
         /* Suppressed by dedup — count it and free the candidate's
          * heap.  admit=false here implies the lookup hit (the only
          * !admit path above).  Breakdown stays in place; effectively
          * garbage past `kept` after compaction. */
         (*out_suppressed)++;
         free(c->text);
         free(c->item_id);
         c->text = NULL;
         c->item_id = NULL;
      }
   }

   result->candidate_count = kept;
}

int build_focus_block(int user_id,
                      int64_t conv_id,
                      int64_t turn_id,
                      const char *user_turn_text,
                      char **out_block) {
   if (out_block == NULL)
      return FAILURE;
   *out_block = NULL;

   /* Snapshot the focus_injection sub-config under one read — guards
    * against TOCTOU when the WebUI settings handler mutates `enabled`
    * + `top_k` from a different thread mid-call (security audit M1).
    * Inexpensive struct copy — current shape is a few ints + floats +
    * the dedup sub-struct. */
   const focus_injection_config_t fi = g_config.memory.focus_injection;

   /* Gate 1: feature flag.  ZERO embedding compute, ZERO focus_compose
    * call when disabled.  This is the absolute top-of-function gate
    * per Phase 1e invariant 6. */
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
    *
    * The 8 KB query_embed buffer must outlive the embed call all the
    * way through focus_compose (adapters read it synchronously), so
    * it stays at function scope.  The early-out below skips embedding
    * AND focus_compose vector adapters when the engine is unavailable
    * — the storage cost is paid (8 KB stack on the worker thread),
    * but the embed/compose CPU cost is not.  Default Linux pthread
    * stacks are 8 MB so the headroom is fine.  TODO(perf): if a
    * future constrained-stack worker needs this, switch to a thread-
    * local heap buffer. */
   float query_embed[MAX_EMBEDDING_DIMS];
   int embed_dims = 0;
   const float *query_ptr = NULL;
   if (memory_embeddings_available()) {
      if (memory_embeddings_embed(user_turn_text, query_embed, &embed_dims) == SUCCESS &&
          embed_dims > 0) {
         query_ptr = query_embed;
      } else {
         OLOG_WARNING("focus: embedding compute failed for user_id=%d — "
                      "passing NULL embedding to focus_compose (vector adapters skip)",
                      user_id);
         embed_dims = 0;
      }
   }
   /* Defensive clamp — security audit L2: any backend that returned a
    * negative dim would wrap to a huge size_t after the cast and walk
    * focus_compose into undefined memory. */
   if (embed_dims < 0)
      embed_dims = 0;

   const time_t now = time(NULL);
   const int top_k = fi.top_k > 0 ? fi.top_k : 8;
   const int per_source_max = compute_per_source_max(top_k, fi.dedup.recent_window_turns);

   focus_compose_result_t result = { 0 };
   const int rc = focus_compose(user_id, /*include_private*/ false, user_turn_text, query_ptr,
                                (size_t)embed_dims, now, per_source_max, &result);
   if (rc != SUCCESS) {
      OLOG_WARNING("focus: focus_compose failed (user_id=%d)", user_id);
      focus_result_free(&result);
      return FAILURE;
   }

   const int candidates_before_dedup = result.candidate_count;
   int dedup_suppressed = 0;

   /* Phase 1f: dedup runs only when a session is published into the
    * dispatch TLS slot — i.e., we're on the PER_TURN path.  SESSION_START
    * (refresh_all_prompts) and standalone build_system_prompt_string
    * callers leave the slot NULL; build_focus_block already short-
    * circuits on NULL/empty turn text in those paths anyway, but the
    * NULL guard here is defense-in-depth for any future caller. */
   session_t *dedup_session = session_get_dispatch_session();
   bool warn_all_suppressed = false;
   if (dedup_session != NULL && result.candidate_count > 0) {
      pthread_mutex_lock(&dedup_session->history_mutex);
      apply_dedup_locked(dedup_session, &result, top_k, fi.dedup.recent_window_turns, fi.min_score,
                         fi.dedup.score_uplift_factor, &dedup_suppressed);
      /* Empty-block observability: if dedup ate everything this turn
       * AND we had candidates going in, the user might be tuning the
       * window/uplift too aggressively.  Log OLOG_WARNING ONCE per
       * session — gate via the session-scoped flag (under the same
       * lock that owns the flag).  Substitutes for the kill-switch
       * 1e shipped without; runaway suppression is observable without
       * adding a config knob. */
      if (candidates_before_dedup > 0 && result.candidate_count == 0 &&
          !dedup_session->injected_set.all_suppressed_logged_once) {
         dedup_session->injected_set.all_suppressed_logged_once = true;
         warn_all_suppressed = true;
      }
      pthread_mutex_unlock(&dedup_session->history_mutex);

      if (warn_all_suppressed)
         OLOG_WARNING("focus: dedup suppressed all %d candidates this turn (session=%u); "
                      "dedup config may need tuning",
                      candidates_before_dedup, dedup_session->session_id);
   }

   /* Empty result (post-dedup) is a successful no-op for the focus
    * BLOCK — composer omits the section.  But Phase 1g-i still
    * broadcasts the empty payload so the UI shows "DAWN looked,
    * found nothing" rather than silently skipping; the broadcast
    * fires below, after the heap-cheap rejection-logging walk. */
   const bool empty_result = (result.candidate_count <= 0);

   if (!empty_result) {
      /* Render survivors.  Format: one line per candidate, opening with
       * the source_id in brackets so the LLM can attribute relevance.
       * Text content is reproduced verbatim from the candidate (already
       * truncated to FOCUS_TEXT_MAX_BYTES inside the framework).
       *
       * `[system_time]` synthetic entry sits at the top of the block on
       * every PER_TURN refresh — moved here from the retired now_block
       * when the prompt-cache split landed.  Cheap (single strftime);
       * shape tracks the legacy now_block contract: human-readable +
       * ISO 8601 + freshness nudge so the LLM trusts it over a `time`
       * tool call.  Gated on `dedup_session != NULL` (only real per-
       * turn dispatch should see fresh time; SESSION_START and
       * standalone callers leave the slot NULL) and `!empty_result`
       * (preserves the legacy "all suppressed → NULL block"
       * invariant — quiet turns fall back to the time tool same as
       * pre-split). */
      strbuf_t sb;
      strbuf_init_with_max(&sb, FOCUS_BLOCK_INIT_BYTES, FOCUS_BLOCK_MAX_BYTES);
      if (dedup_session != NULL) {
         time_t now_t = time(NULL);
         struct tm tm_storage;
         struct tm *tm_info = localtime_r(&now_t, &tm_storage);
         if (tm_info != NULL) {
            char human[64];
            char iso_local[32];
            char iso_offset[8];
            if (strftime(human, sizeof(human), "%A, %Y-%m-%d %H:%M %Z", tm_info) > 0 &&
                strftime(iso_local, sizeof(iso_local), "%Y-%m-%dT%H:%M:%S", tm_info) > 0 &&
                strftime(iso_offset, sizeof(iso_offset), "%z", tm_info) > 0) {
               char iso_offset_colon[8];
               if (iso_offset[0] != '\0' && (iso_offset[0] == '+' || iso_offset[0] == '-') &&
                   iso_offset[1] != '\0' && iso_offset[2] != '\0' && iso_offset[3] != '\0' &&
                   iso_offset[4] != '\0') {
                  snprintf(iso_offset_colon, sizeof(iso_offset_colon), "%c%c%c:%c%c", iso_offset[0],
                           iso_offset[1], iso_offset[2], iso_offset[3], iso_offset[4]);
               } else {
                  snprintf(iso_offset_colon, sizeof(iso_offset_colon), "Z");
               }
               strbuf_appendf(&sb,
                              "[system_time] Current time: %s (ISO: %s%s).  This timestamp is "
                              "fresh as of this turn; use it for relative-time computations and "
                              "tool args like `fire_at`.  The `time` tool is only needed for "
                              "sub-second precision.\n",
                              human, iso_local, iso_offset_colon);
            }
         }
      }
      /* Memory citation signal (Phase 1): when enabled, number each surfaced
       * memory as [M# source] and stash ordinal->item_id so the response
       * finalizer can resolve a <cited>M#</cited> back to its item.  When
       * disabled, render the plain [source] form (unchanged). */
      const bool citation_on = g_config.memory.citation_enabled;
      citation_stash_t local_stash;
      memset(&local_stash, 0, sizeof(local_stash));
      int m_ordinal = 0;

      for (int i = 0; i < result.candidate_count; i++) {
         const focus_candidate_t *c = &result.candidates[i];
         if (c->text == NULL || c->text[0] == '\0')
            continue;

         /* Number a candidate [M#] ONLY when it is citeable (has an item_id) AND
          * the stash still has room — so every rendered [M#] maps 1:1 to a stash
          * slot.  Non-citeable / overflow candidates render the plain [source]
          * form so the model never sees an [M#] it can't be resolved back to
          * (which would mis-score a real citation as a hallucination). */
         const bool numbered = citation_on && c->item_id != NULL && m_ordinal < MAX_CITATION_STASH;

         int rc_append;
         if (numbered) {
            const int next_ordinal = m_ordinal + 1;
            rc_append = strbuf_appendf(&sb, "[M%d %s] %s\n", next_ordinal, c->source_id, c->text);
            if (rc_append >= 0) {
               /* Commit the ordinal only after the text is in the block. */
               m_ordinal = next_ordinal;
               strncpy(local_stash.entries[m_ordinal - 1].item_id, c->item_id,
                       sizeof(local_stash.entries[0].item_id) - 1);
               local_stash.entries[m_ordinal - 1]
                   .item_id[sizeof(local_stash.entries[0].item_id) - 1] = '\0';
               /* Capture the ranker composite this item was injected at (parallel
                * score_breakdowns[i]); FOCUS_SCORE_NA if the breakdown is absent so
                * the audit can tell "no score recorded" from a real 0. */
               local_stash.entries[m_ordinal - 1].final_score =
                   (result.score_breakdowns != NULL) ? result.score_breakdowns[i].final_score
                                                     : FOCUS_SCORE_NA;
               local_stash.count = m_ordinal;
            }
         } else {
            rc_append = strbuf_appendf(&sb, "[%s] %s\n", c->source_id, c->text);
         }

         if (rc_append < 0) {
            /* strbuf max-cap hit — stop appending; surface the partial block so
             * the LLM still sees the highest-ranked items. */
            OLOG_WARNING("focus: strbuf max cap reached at candidate %d/%d — truncating", i,
                         result.candidate_count);
            break;
         }
      }

      /* Publish the per-turn citation map onto the dispatch session.  PER_TURN
       * path only (dedup_session != NULL); SESSION_START/standalone builds leave
       * it NULL and need no stash.  Cleared at dispatch entry, so a short-
       * circuited later turn cannot inherit this map. */
      if (citation_on && dedup_session != NULL) {
         pthread_mutex_lock(&dedup_session->history_mutex);
         dedup_session->citation_stash = local_stash;
         pthread_mutex_unlock(&dedup_session->history_mutex);
      }

      /* Lift ownership of the strbuf-internal buffer into out_block.
       * Use _or_null variant so an empty buffer (every candidate had
       * empty text — pathological but possible) collapses to NULL and
       * the composer omits the section.  strbuf_steal resets the buf
       * to its zero state, so strbuf_free is safe + a no-op afterward. */
      *out_block = strbuf_steal_or_null(&sb);
      strbuf_free(&sb);

      /* Per-source rejection logging — fires only when the framework
       * filter caught poison in adapter output. */
      for (int i = 0; i < result.rejection_count; i++) {
         const focus_filter_rejection_t *r = &result.rejections[i];
         if (r->count > 0)
            OLOG_WARNING("focus: filter rejected %d candidate(s) from source='%s' (user_id=%d)",
                         r->count, r->source_id, user_id);
      }
   }

   /* Phase 1g-i: broadcast the structured event to every WebUI session
    * matching (user_id, conv_id) — fires on EVERY post-dedup result
    * including empty (empty-state UX), but ONLY when the feature is
    * enabled (already gated above) and conv_id is positive.  The
    * helper itself filters to SESSION_TYPE_WEBUI sessions; non-webui
    * dispatchers (DAP / DAP2 / LOCAL) reach this point with conv_id
    * > 0 too, but the type gate is what keeps the event scoped to
    * browser tabs that actually consume it.  history_mutex was
    * released back at line ~346; broadcast iterates a separate
    * registry mutex.  conv_id == 0 disables — the SESSION_START
    * refresh_all_prompts path passes 0 (no user-visible turn).
    *
    * First-turn race fix: dawn_build_prompt() captures conv_id near
    * the top of prompt assembly, but on a fresh-chat first turn the
    * WebUI client sends `new_conversation` AFTER `text_input` — the
    * main thread's handler may not have set `active_conversation_id`
    * on the connection yet at the moment the worker thread captured
    * it (observed live: ~47ms gap between text_input dispatch and
    * new_conversation handler completing).  By the time we reach this
    * broadcast site, the embed + focus_compose + dedup work has
    * elapsed ~100ms+, more than enough for the main thread to have
    * finished the parallel new_conversation request.  Re-read from
    * the dispatch session here and prefer the live value; fall back
    * to the captured conv_id when the live read is still 0 (e.g.
    * SESSION_START refresh paths that legitimately have no active
    * conversation, or tests with no dispatch session published). */
   int64_t broadcast_conv_id = conv_id;
   session_t *dispatch = session_get_dispatch_session();
   if (dispatch != NULL) {
      int64_t live_conv = webui_get_active_conversation_id(dispatch);
      if (live_conv > 0)
         broadcast_conv_id = live_conv;
   }
   if (broadcast_conv_id > 0)
      webui_broadcast_context_injection(user_id, broadcast_conv_id, turn_id, &result);

   log_focus_summary(user_turn_text, result.candidate_count, result.rejection_count,
                     dedup_suppressed, monotonic_ms_now() - t_start);

   focus_result_free(&result);
   return SUCCESS;
}
