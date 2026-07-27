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
 * Memory Callback Implementation
 *
 * Handles the memory tool actions: search, remember, forget.
 */

#define _GNU_SOURCE /* strptime + timegm for as_of date parsing */

#include <ctype.h>
#include <errno.h>
#include <json-c/json.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/iso8601.h"
#include "core/session_manager.h"
#include "core/strbuf.h"
#include "core/time_query_parser.h"
#include "logging.h"
#include "memory/contacts_db.h"
#include "memory/memory_callback_internal.h"
#include "memory/memory_db.h"
#include "memory/memory_db_aliases.h"
#include "memory/memory_db_provenance.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_fact_search.h"
/* SOURCE_DEDUP_CAP / source_dedup_set_t / source_dedup_{seen,add} live in
 * memory_callback_internal.h so the unit tests can exercise them directly. */
#include "core/memory_filter.h"
#include "memory/memory_similarity.h"
#include "memory/memory_stem.h"
#include "memory/memory_types.h"
#include "mosquitto_comms.h"
#include "tools/time_utils.h"
#include "tools/tool_registry.h"

/* Shared char budget for with_source verbatim excerpts in a single tool call.
 * The response buffer is now growable (strbuf, capped at STRBUF_DEFAULT_MAX_CAP =
 * 256 KiB) so this is no longer a "stay under buffer size" invariant — it is
 * a soft cap on how much verbatim source text any one memory_callback call
 * can inject into the LLM context.  3072 chars ≈ 768 tokens; bench validation
 * (May 2026) showed 6144 / 12288 lift answer quality further but cost more
 * context window per call.  Tune via dawn.toml [memory] source_budget_chars
 * when production telemetry justifies it. */
#define MEMORY_SOURCE_BUDGET_CHARS 3072

/* Hard upper bound on per-call source_budget — prevents a misconfigured
 * caller (LLM tool param escape, future admin path, etc.) from injecting
 * an arbitrary amount of verbatim source text and dominating the LLM
 * context.  Defense-in-depth — the bench legitimately needs values up to
 * ~12 KiB for the budget sweep, so the cap is set well above that. */
#define MEMORY_SOURCE_BUDGET_MAX (32 * 1024)

/* Per-fact cap on how many messages from a fact's provenance range are
 * emitted as verbatim source excerpts.  Replaces the pre-2026-05-14 behavior
 * that dumped EVERY message in the range — for facts whose provenance spans
 * a whole session (e.g., msgs=59-76, 18 messages) that produced ~10 KB of
 * mostly off-topic dialog per fact and the generator (gpt-4o-mini under the
 * Mem0 protocol's <6-word constraint) refused to answer ~60% of the time
 * despite the gold answer being present in the excerpt.  Top-K selection
 * shrinks per-fact noise; ranking by (query + fact_text) token overlap
 * surfaces the dialog turn(s) most likely to evidence the fact.  Bench
 * validated on LoCoMo 1536 QA — see /tmp/locomo_failures.jsonl analysis. */
#define MEMORY_SOURCE_TOP_MESSAGES 3

/* =============================================================================
 * Helper: Append verbatim source excerpt for a (conv_id, msg-range) tuple
 * (with_source=true).
 *
 * Appends "[source: conv=N msgs=X-Y]\n<role>: <content>\n..." to buf.
 * Respects the shared budget; returns 1 if anything rendered, 0 otherwise.
 *
 * Selection: up to MEMORY_SOURCE_TOP_MESSAGES messages from the range are
 * emitted, picked by case-insensitive substring overlap against the
 * (query + fact_text) token set.  When that score set is empty (or all
 * messages tie at zero), insertion order wins — the first K user/assistant
 * messages of the range.  Tool / system messages are skipped.  Emitted
 * messages are re-sorted into original dialog order so the excerpt reads
 * coherently regardless of which K were picked.
 *
 * Dedup: when a non-NULL source_dedup_set_t is passed, repeated ranges emit a
 * single back-ref line ("[source: same as above ...]") instead of refetching
 * the message body.  Triples are added to the set ONLY after a successful
 * fetch through the privacy-filtered path — so a private upstream cannot
 * leak its existence as a "same as above" marker on a later public record.
 * ============================================================================= */

/* Forward decl — defined later in this file. */
static int tokenize_query(const char *keywords, char tokens[][64], int max_tokens);

struct top_k_slot {
   int score; /* token-overlap score */
   int order; /* position in the original message stream, for stable emit order */
   char role[16];
   char content[520];
};

struct top_k_collector {
   struct top_k_slot slots[MEMORY_SOURCE_TOP_MESSAGES];
   int filled;
   int min_idx;        /* index of lowest-score slot, recomputed on each replace */
   char tokens[8][64]; /* MAX_SEARCH_TOKENS from tokenize_query; redeclared here so this block
                          precedes its definition */
   int n_tokens;
   int seen; /* count of user/assistant messages observed; monotonic emit order */
};

static int score_against_tokens(const char *content, const char tokens[][64], int n_tokens) {
   if (n_tokens <= 0 || !content)
      return 0;
   int score = 0;
   for (int i = 0; i < n_tokens; i++) {
      if (tokens[i][0] && strcasestr(content, tokens[i]))
         score++;
   }
   return score;
}

static int collect_top_k_callback(const conversation_message_t *msg, void *ctx_ptr) {
   struct top_k_collector *ctx = (struct top_k_collector *)ctx_ptr;
   if (!ctx || !msg || !msg->content)
      return 1;
   if (strcmp(msg->role, "user") != 0 && strcmp(msg->role, "assistant") != 0)
      return 0;

   int score = score_against_tokens(msg->content, ctx->tokens, ctx->n_tokens);
   int order = ctx->seen++;

   if (ctx->filled < MEMORY_SOURCE_TOP_MESSAGES) {
      struct top_k_slot *s = &ctx->slots[ctx->filled];
      s->score = score;
      s->order = order;
      snprintf(s->role, sizeof(s->role), "%s", msg->role);
      snprintf(s->content, sizeof(s->content), "%s", msg->content);
      ctx->filled++;
      if (ctx->filled == MEMORY_SOURCE_TOP_MESSAGES) {
         ctx->min_idx = 0;
         for (int i = 1; i < MEMORY_SOURCE_TOP_MESSAGES; i++) {
            if (ctx->slots[i].score < ctx->slots[ctx->min_idx].score)
               ctx->min_idx = i;
         }
      }
      return 0;
   }

   /* Strictly greater so an early high-scoring message holds its slot
    * against a later tie — preserves first-occurrence preference on ties. */
   if (score > ctx->slots[ctx->min_idx].score) {
      struct top_k_slot *s = &ctx->slots[ctx->min_idx];
      s->score = score;
      s->order = order;
      snprintf(s->role, sizeof(s->role), "%s", msg->role);
      snprintf(s->content, sizeof(s->content), "%s", msg->content);
      ctx->min_idx = 0;
      for (int i = 1; i < MEMORY_SOURCE_TOP_MESSAGES; i++) {
         if (ctx->slots[i].score < ctx->slots[ctx->min_idx].score)
            ctx->min_idx = i;
      }
   }
   return 0;
}

static int slot_order_cmp(const void *a, const void *b) {
   const struct top_k_slot *aa = (const struct top_k_slot *)a;
   const struct top_k_slot *bb = (const struct top_k_slot *)b;
   return aa->order - bb->order;
}

/* source_dedup_seen / source_dedup_add live in memory_source_dedup.c so unit
 * tests can link them without the full memory_callback dependency cone. */

/* Core renderer — caller passes a (conv_id, range) triple already filtered
 * upstream (typically from a *_get_sources batch API which excludes private
 * conversations in SQL).  Caller is responsible for passing conv_id > 0;
 * conv_id = 0 means "no provenance" and we silently skip.
 *
 * `query` and `fact_text` are optional scoring signals.  When non-NULL, they
 * are concatenated and tokenized so messages in the range can be ranked by
 * substring overlap.  When both are NULL (or yield zero tokens), the first
 * K user/assistant messages of the range are emitted in original order.
 *
 * `seen` may be NULL to disable dedup. */
static int append_source_excerpt_from_range(int user_id,
                                            int64_t conv_id,
                                            int64_t start_id,
                                            int64_t end_id,
                                            const char *query,
                                            const char *fact_text,
                                            strbuf_t *sb,
                                            int *budget_remaining,
                                            source_dedup_set_t *seen) {
   if (!sb || !budget_remaining)
      return 0;
   if (*budget_remaining <= 0)
      return 0;
   if (conv_id <= 0)
      return 0;

   /* Dedup: emit a one-line back-ref instead of refetching identical messages.
    * `seen` is populated only with post-batch-filter triples (see Sec M1 in
    * PROVENANCE.md) so this path cannot leak existence of a private upstream. */
   if (source_dedup_seen(seen, conv_id, start_id, end_id)) {
      int hdr = strbuf_appendf(sb, "[source: same as above (conv=%lld msgs=%lld-%lld)]\n",
                               (long long)conv_id, (long long)start_id, (long long)end_id);
      if (hdr > 0)
         *budget_remaining -= hdr;
      return 1;
   }

   /* Build scoring token set from (query + fact_text).  Either may be NULL.
    * 512 chars is plenty for a tokenizer that hard-caps at 8 tokens of ≤64
    * chars; tokenize_query truncates internally if its 512-byte buffer
    * overflows.  If both inputs are absent, n_tokens stays 0 and the
    * top-K collector degrades to first-K insertion order. */
   struct top_k_collector coll = { 0 };
   if ((query && *query) || (fact_text && *fact_text)) {
      char score_buf[512];
      int q_len = (query && *query) ? snprintf(score_buf, sizeof(score_buf), "%s", query) : 0;
      if (q_len < 0)
         q_len = 0;
      if (fact_text && *fact_text && (size_t)q_len < sizeof(score_buf) - 1) {
         if (q_len > 0 && (size_t)q_len < sizeof(score_buf) - 1) {
            score_buf[q_len++] = ' ';
            score_buf[q_len] = '\0';
         }
         snprintf(score_buf + q_len, sizeof(score_buf) - (size_t)q_len, "%s", fact_text);
      }
      coll.n_tokens = tokenize_query(score_buf, coll.tokens,
                                     (int)(sizeof(coll.tokens) / sizeof(coll.tokens[0])));
   }

   /* Collect candidates — cap range at 500 messages per dedup spec; the
    * top-K collector reduces the emit set further to MEMORY_SOURCE_TOP_MESSAGES. */
   int64_t capped_end = (end_id - start_id > 500) ? start_id + 500 : end_id;
   conv_db_get_messages_by_range(conv_id, user_id, start_id, capped_end, /*include_private=*/false,
                                 collect_top_k_callback, &coll);

   if (coll.filled == 0) {
      /* No emittable user/assistant messages in this range (e.g., the
       * provenance pointed at tool/system messages only).  Skip header
       * AND skip dedup — a later identical range with real content should
       * still get a chance to render. */
      return 0;
   }

   /* Sort picks back into original dialog order so the excerpt reads
    * coherently even though we cherry-picked by score. */
   if (coll.filled > 1)
      qsort(coll.slots, (size_t)coll.filled, sizeof(struct top_k_slot), slot_order_cmp);

   /* Header — emitted only when we have something to render under it. */
   int hdr = strbuf_appendf(sb, "[source: conv=%lld msgs=%lld-%lld]\n", (long long)conv_id,
                            (long long)start_id, (long long)end_id);
   if (hdr > 0)
      *budget_remaining -= hdr;

   for (int i = 0; i < coll.filled; i++) {
      if (*budget_remaining <= 0)
         break;
      int written = strbuf_appendf(sb, "%s: %.500s\n", coll.slots[i].role, coll.slots[i].content);
      if (written < 0) {
         /* STRBUF_E_OOM — buffer cap exhausted.  Drain budget so the
          * caller's outer loop also bails (memory_action_search checks
          * strbuf_oom after each excerpt, but zeroing budget short-circuits
          * any other consumers that watch budget rather than oom). */
         *budget_remaining = 0;
         break;
      }
      *budget_remaining -= written;
   }

   /* Mark range as seen — only after a successful (privacy-filtered) fetch. */
   source_dedup_add(seen, conv_id, start_id, end_id);
   return 1;
}

/* (Phase B fold-in removed the single-fact `append_source_excerpt` wrapper —
 * all production paths batch-fetch sources via the `_get_sources` APIs and
 * call `append_source_excerpt_from_range` directly.  The single-record
 * `memory_db_fact_get_source` is still used by the WebUI memory panel
 * endpoint in webui_memory.c, which doesn't render the excerpt itself.) */

/* =============================================================================
 * Helper: Get user ID from current session
 * ============================================================================= */

static int get_current_user_id(void) {
#ifdef ENABLE_MULTI_CLIENT
   session_t *session = session_get_command_context();
   if (session) {
      /* For authenticated WebSocket sessions, use their user_id */
      if (session->metrics.user_id > 0) {
         return session->metrics.user_id;
      }
      /* For local voice sessions, use configured default user */
      if (session->type == SESSION_TYPE_LOCAL) {
         int user_id = g_config.memory.default_voice_user_id;
         return (user_id > 0) ? user_id : 1; /* Fallback to admin */
      }
   }
#endif
   /* Fallback for non-multi-client builds: use default voice user */
   int user_id = g_config.memory.default_voice_user_id;
   return (user_id > 0) ? user_id : 1;
}

/* =============================================================================
 * Helper: Format time difference
 * ============================================================================= */

static void format_time_ago(time_t timestamp, char *buf, size_t buf_size) {
   if (timestamp == 0) {
      snprintf(buf, buf_size, "unknown");
      return;
   }

   time_t now = time(NULL);
   time_t diff = now - timestamp;

   if (diff < 60) {
      snprintf(buf, buf_size, "just now");
   } else if (diff < 3600) {
      snprintf(buf, buf_size, "%ld min ago", diff / 60);
   } else if (diff < 86400) {
      snprintf(buf, buf_size, "%ld hours ago", diff / 3600);
   } else if (diff < 604800) {
      snprintf(buf, buf_size, "%ld days ago", diff / 86400);
   } else {
      snprintf(buf, buf_size, "%ld weeks ago", diff / 604800);
   }
}

/* =============================================================================
 * Helper: Tokenize search query into individual words
 *
 * Splits keywords on whitespace/punctuation, lowercases each token,
 * and skips single-character tokens (noise). Returns token count.
 * ============================================================================= */

#define MAX_SEARCH_TOKENS 8

/* `multi_token_fact_search` was extracted to `src/memory/memory_fact_search.c`
 * (Phase 1c-i) so the focus-source fact adapter and the legacy recall path
 * share identical retrieval semantics.  `tokenize_query` here is now a thin
 * wrapper over the shared `memory_stem_tokenize_in_place` helper — same
 * delimiter set, min-length=2 filter, and working-buffer size as
 * memory_fact_search.c::tokenize_query.  Kept file-local for caller
 * convenience (fills the existing `char tokens[][64]` storage shape used by
 * downstream search calls). */

static int tokenize_query(const char *keywords, char tokens[][64], int max_tokens) {
   int cap = (max_tokens > MAX_SEARCH_TOKENS) ? MAX_SEARCH_TOKENS : max_tokens;
   return memory_stem_tokenize_padded(keywords, tokens, cap, 2);
}

/* =============================================================================
 * Helper: Append entity graph context to search results
 *
 * as_of_ts (v33): timestamp at which to evaluate relation validity.
 *   0 = "currently true" (now()).
 *   non-zero = relations valid at that historical timestamp.
 * include_historical: when true, bypasses validity filter (returns all rows).
 * ============================================================================= */

static void append_graph_context(int user_id,
                                 const char *keywords,
                                 int64_t as_of_ts,
                                 bool include_historical,
                                 strbuf_t *sb,
                                 bool with_source,
                                 int *source_budget,
                                 source_dedup_set_t *seen) {
   /* Try semantic entity search first */
   int64_t entity_ids[5];
   char entity_names[5][MEMORY_ENTITY_NAME_MAX];
   char entity_types[5][MEMORY_ENTITY_TYPE_MAX];
   float entity_scores[5];
   int entity_count = 0;

   entity_count = memory_embeddings_entity_search(user_id, keywords, NULL, entity_ids, entity_names,
                                                  entity_types, entity_scores, 5);

   /* Fallback to keyword search if vector search returned nothing */
   if (entity_count == 0) {
      /* Tokenize query and search per-token to avoid full-string LIKE mismatch */
      char tokens[MAX_SEARCH_TOKENS][64];
      int token_count = tokenize_query(keywords, tokens, MAX_SEARCH_TOKENS);
      int64_t seen_ids[5];
      int seen_count = 0;

      for (int t = 0; t < token_count && entity_count < 5; t++) {
         memory_entity_t kw_entities[5];
         int kw_count = 0;
         memory_db_entity_search(user_id, tokens[t], kw_entities, 5, &kw_count);
         for (int i = 0; i < kw_count && entity_count < 5; i++) {
            /* Dedup by entity ID */
            bool dup = false;
            for (int k = 0; k < seen_count; k++) {
               if (seen_ids[k] == kw_entities[i].id) {
                  dup = true;
                  break;
               }
            }
            if (dup)
               continue;
            if (seen_count < 5)
               seen_ids[seen_count++] = kw_entities[i].id;
            entity_ids[entity_count] = kw_entities[i].id;
            strncpy(entity_names[entity_count], kw_entities[i].name, MEMORY_ENTITY_NAME_MAX - 1);
            entity_names[entity_count][MEMORY_ENTITY_NAME_MAX - 1] = '\0';
            strncpy(entity_types[entity_count], kw_entities[i].entity_type,
                    MEMORY_ENTITY_TYPE_MAX - 1);
            entity_types[entity_count][MEMORY_ENTITY_TYPE_MAX - 1] = '\0';
            entity_count++;
         }
      }
   }

   if (entity_count == 0)
      return;

   /* Show top 3 entities with their relations (skip entities with no relations) */
   int show_count = entity_count > 3 ? 3 : entity_count;
   bool header_written = false;

   /* Hoisted out of the entity loop so we don't pay 3× zero-init cost per
    * call.  reset to zeros only when we actually batch-fetch (embedded MED). */
   int64_t out_conv[8], out_start[8], out_end[8];
   int64_t rel_ids[8];

   for (int i = 0; i < show_count; i++) {
      /* Pre-fetch relations to check if entity has any.  When include_historical
       * is true, return all relations regardless of validity.  Otherwise apply
       * temporal filtering: relations valid at as_of_ts (0 = now). */
      memory_relation_t rels[8];
      int rel_count;
      if (include_historical) {
         memory_db_relation_list_by_subject(user_id, entity_ids[i], rels, 8, &rel_count);
      } else {
         memory_db_relation_list_by_subject_at(user_id, entity_ids[i], as_of_ts, rels, 8,
                                               &rel_count);
      }
      /* Incoming relations: no temporal filter helper today; subject-side filter is
       * the high-value case (e.g., "where does Alice work").  Object-side stays
       * unfiltered for v1 — incoming recall is contextual, less likely to confuse. */
      memory_relation_t in_rels[8];
      int in_count = 0;
      memory_db_relation_list_by_object(user_id, entity_ids[i], in_rels, 8, &in_count);

      if (rel_count == 0 && in_count == 0)
         continue;

      /* Write header on first entity that has relations */
      if (!header_written) {
         if (strbuf_len(sb) > 0)
            strbuf_append(sb, "\n");
         strbuf_append(sb, "ENTITIES:\n");
         header_written = true;
      }

      strbuf_appendf(sb, "- %s (%s):\n", entity_names[i], entity_types[i]);

      /* Batch-fetch source ranges for this entity's outgoing relations so we
       * can render verbatim excerpts inline.  Caller decides whether to render
       * via with_source / *source_budget; we still skip the DB hit when
       * source rendering is disabled. */
      bool render_rel_source = with_source && source_budget && *source_budget > 0;
      if (render_rel_source && rel_count > 0) {
         /* Zero only the slots we'll consult for this entity. */
         for (int r = 0; r < rel_count; r++) {
            out_conv[r] = 0;
            out_start[r] = 0;
            out_end[r] = 0;
            rel_ids[r] = rels[r].id;
         }
         memory_db_relations_get_sources(user_id, rel_ids, rel_count, out_conv, out_start, out_end);
      }

      for (int r = 0; r < rel_count; r++) {
         strbuf_appendf(sb, "  %s: %s\n", rels[r].relation, rels[r].object_name);
         if (render_rel_source && out_conv[r] > 0 && *source_budget > 0) {
            append_source_excerpt_from_range(user_id, out_conv[r], out_start[r], out_end[r],
                                             keywords, rels[r].object_name, sb, source_budget,
                                             seen);
            if (strbuf_oom(sb))
               return;
         }
      }

      for (int r = 0; r < in_count; r++) {
         strbuf_appendf(sb, "  %s %s %s\n", in_rels[r].object_name, in_rels[r].relation,
                        entity_names[i]);
      }
   }
}

/* =============================================================================
 * Action: Search
 * ============================================================================= */

/* category (v34): when non-NULL/non-empty, pre-filters fact-ID set by exact category
 *   match before hybrid scoring.  Bypasses time_range path (categories layer above
 *   recency for now — combinable in a follow-up if useful).
 * as_of_ts (v33): timestamp for relation validity in entity recall (0 = now).
 * include_historical (v33): bypass relation validity filter entirely. */
char *memory_action_search(int user_id,
                           const char *keywords,
                           time_t since_ts,
                           const char *category,
                           int64_t as_of_ts,
                           bool include_historical,
                           bool with_source,
                           int source_budget) {
   if (!keywords || strlen(keywords) == 0) {
      return strdup("Please provide search keywords.");
   }

   /* Growable response buffer.  8KB initial sizes for the typical case (small
    * source budget, modest entity graph); strbuf doubles as needed so neither
    * a higher source_budget nor a denser conversation can silently truncate
    * facts/prefs/summaries the way the previous fixed 8KB did.  Cap at the
    * default (256 KiB) — well above any realistic memory tool response. */
   strbuf_t sb;
   strbuf_init(&sb, 8192);

   /* Tokenize query for per-word matching */
   char tokens[MAX_SEARCH_TOKENS][64];
   int token_count = tokenize_query(keywords, tokens, MAX_SEARCH_TOKENS);

   /* Search facts — keyword search first, then hybrid if embeddings available */
   memory_fact_t facts[10];
   int fact_count = 0;
   /* Category hard-filtering removed (was: pre-filter facts by exact category via the
    * now-deleted memory_db_fact_search_by_category).  Extraction scatters one semantic cluster
    * across multiple categories, so an LLM-guessed `category` systematically hid
    * relevant facts (e.g. a single task list filed under general + professional +
    * practical at once).  Always use the unified full-corpus path —
    * `memory_search_execute` does hybrid + entity-graph + Phase 2 Step 1
    * query-aware scoring + merge + score floor in one call, shared with the bench
    * harness to prevent drift, and it strictly subsumes the old branch.
    * `category` is still accepted but no longer narrows search results. */
   (void)category;
   float scores[10];
   memory_search_execute(user_id, keywords, since_ts, facts, scores, 10, &fact_count);

   /* source_budget==0 from caller means "use default"; else honor caller's value
    * (bench harness uses this to sweep budget values).  Cap at
    * MEMORY_SOURCE_BUDGET_MAX so a misconfigured caller cannot inject an
    * arbitrary amount of verbatim source text — defense-in-depth.  The
    * bench is the only legitimate caller passing > 3072 today; if a
    * future LLM-tool exposes the param to model output, the clamp here
    * is the backstop. */
   if (source_budget <= 0)
      source_budget = MEMORY_SOURCE_BUDGET_CHARS;
   else if (source_budget > MEMORY_SOURCE_BUDGET_MAX)
      source_budget = MEMORY_SOURCE_BUDGET_MAX;

   /* Source-fetch dedup set spans the whole call: same (conv, range) cited by
    * a fact + a summary + a relation emits "same as above" after the first
    * verbatim. */
   source_dedup_set_t seen = { 0 };

   /* Minimal-format mode (v5 memory_text shape rebuild).  When enabled,
    * strip per-fact metadata, demote source excerpts to a single global
    * "Source evidence:" block at the end (top-3 facts), and skip the
    * entity-graph block.  Goal: ~2K tokens vs current ~3K.  Off-by-default. */
   const bool minimal = g_config.memory.memory_text_minimal;

   /* Captured provenance for the deferred global source block (minimal mode).
    * Only the first 3 facts contribute — globally bounded so source overhead
    * stays roughly constant regardless of fact_count. */
   enum {
      MINIMAL_SOURCE_TOP_FACTS = 3
   };
   int64_t min_src_conv[MINIMAL_SOURCE_TOP_FACTS] = { 0 };
   int64_t min_src_start[MINIMAL_SOURCE_TOP_FACTS] = { 0 };
   int64_t min_src_end[MINIMAL_SOURCE_TOP_FACTS] = { 0 };
   const char *min_src_fact_text[MINIMAL_SOURCE_TOP_FACTS] = { NULL };
   int min_src_count = 0;

   if (fact_count > 0) {
      strbuf_append(&sb, minimal ? "Memories:\n" : "");
      if (!minimal)
         strbuf_appendf(&sb, "FACTS (%d):\n", fact_count);

      /* Batch-fetch fact provenance up front — one DB round-trip instead of
       * N × _fact_get_source calls.  fact_conv[i] = 0 for facts with no
       * provenance recorded or whose source is in a private conversation
       * (privacy filtered in SQL). */
      int64_t fact_conv[10] = { 0 }, fact_start[10] = { 0 }, fact_end[10] = { 0 };
      if (with_source) {
         int64_t fact_ids[10];
         for (int i = 0; i < fact_count; i++)
            fact_ids[i] = facts[i].id;
         memory_db_facts_get_sources(user_id, fact_ids, fact_count, fact_conv, fact_start,
                                     fact_end);
      }

      /* Hard temporal filter (off-by-default; tier-1 candidate from the
       * 2026-05-15 research synthesis).  When the query contains a parsed
       * temporal expression, DROP retrieved facts whose source conversation's
       * anchor_date is outside the parsed window.  Distinct from
       * `temporal_weight` (soft Gaussian decay on fact.created_at): this
       * filter targets conv.anchor_date (when the user *spoke* about the
       * event), which is a better proxy for event time than extraction
       * time.  Safety: if hard-drop would leave fewer than MIN_KEEP facts,
       * keep all (parser may have misread the date; better to surface
       * everything than starve the LLM).  Mem0 v2 empirical lift +29.6pp
       * on temporal questions came from this style of hard metadata filter
       * (vs DAWN's prior soft Gaussian weight). */
      enum {
         MEMORY_TEMPORAL_FILTER_MIN_KEEP = 3
      };
      if (g_config.memory.temporal_filter_enabled && with_source && fact_count > 1 && keywords &&
          *keywords) {
         time_query_t tq = { 0 };
         if (time_query_parse(keywords, (int64_t)time(NULL), &tq) == 0 && tq.found &&
             tq.target_ts > 0 && tq.window_seconds > 0) {
            int64_t t_lo = tq.target_ts - tq.window_seconds;
            int64_t t_hi = tq.target_ts + tq.window_seconds;
            /* Per-fact anchor lookup — N=fact_count ≤ 10, ~1ms each on
             * Jetson NVMe, no batch helper needed at this scale. */
            int in_idx[10];
            int in_count = 0;
            for (int i = 0; i < fact_count; i++) {
               int64_t anchor = 0;
               if (fact_conv[i] > 0 && conv_db_get_anchor_date(fact_conv[i], &anchor) == 0 &&
                   anchor > 0 && anchor >= t_lo && anchor <= t_hi) {
                  in_idx[in_count++] = i;
               }
            }
            /* Apply hard drop only if it leaves >= MIN_KEEP facts. */
            if (in_count >= MEMORY_TEMPORAL_FILTER_MIN_KEEP && in_count < fact_count) {
               memory_fact_t tmp_facts[10];
               int64_t tmp_conv[10], tmp_start[10], tmp_end[10];
               for (int i = 0; i < in_count; i++) {
                  tmp_facts[i] = facts[in_idx[i]];
                  tmp_conv[i] = fact_conv[in_idx[i]];
                  tmp_start[i] = fact_start[in_idx[i]];
                  tmp_end[i] = fact_end[in_idx[i]];
               }
               for (int i = 0; i < in_count; i++) {
                  facts[i] = tmp_facts[i];
                  fact_conv[i] = tmp_conv[i];
                  fact_start[i] = tmp_start[i];
                  fact_end[i] = tmp_end[i];
               }
               fact_count = in_count;
            }
         }
      }

      int elided = 0;
      for (int i = 0; i < fact_count; i++) {
         if (minimal) {
            /* Stripped per-fact line: just `- text`.  Date stays in fact_text
             * when extraction included it (Phase 0 prompt v2). */
            strbuf_appendf(&sb, "- [ID:%lld] %s\n", (long long)facts[i].id, facts[i].fact_text);
            /* Capture provenance for the deferred global Source evidence block. */
            if (with_source && fact_conv[i] > 0 && min_src_count < MINIMAL_SOURCE_TOP_FACTS) {
               min_src_conv[min_src_count] = fact_conv[i];
               min_src_start[min_src_count] = fact_start[i];
               min_src_end[min_src_count] = fact_end[i];
               min_src_fact_text[min_src_count] = facts[i].fact_text;
               min_src_count++;
            }
         } else {
            char time_str[32];
            format_time_ago(facts[i].created_at, time_str, sizeof(time_str));
            strbuf_appendf(&sb, "- [ID:%lld] %s (confidence: %.0f%%, %s)\n", (long long)facts[i].id,
                           facts[i].fact_text, facts[i].confidence * 100, time_str);

            if (with_source && source_budget > 0 && fact_conv[i] > 0) {
               append_source_excerpt_from_range(user_id, fact_conv[i], fact_start[i], fact_end[i],
                                                keywords, facts[i].fact_text, &sb, &source_budget,
                                                &seen);
            } else if (with_source && source_budget <= 0) {
               elided++;
            }
         }

         /* Update access time */
         memory_db_fact_update_access(facts[i].id, user_id);

         /* Bail early if buffer max_cap was hit during source-excerpt
          * rendering.  Otherwise we'd burn N-1 wasted iterations on no-op
          * append_source_excerpt_from_range calls (embedded-review M4). */
         if (strbuf_oom(&sb))
            break;
      }
      if (elided > 0) {
         strbuf_appendf(&sb, "[%d more source(s) elided — call context_expand for full]\n", elided);
      }
   }

   /* Search preferences — SQL-filtered by LIKE on category and value */
   memory_preference_t prefs[10];
   int pref_count = 0;
   memory_db_pref_search(user_id, keywords, prefs, 10, &pref_count);

   if (pref_count > 0) {
      if (strbuf_len(&sb) > 0)
         strbuf_append(&sb, "\n");
      strbuf_append(&sb, minimal ? "Preferences:\n" : "PREFERENCES:\n");

      int64_t pref_conv[10] = { 0 }, pref_start[10] = { 0 }, pref_end[10] = { 0 };
      if (with_source && !minimal) {
         int64_t pref_ids[10];
         for (int i = 0; i < pref_count; i++)
            pref_ids[i] = prefs[i].id;
         memory_db_prefs_get_sources(user_id, pref_ids, pref_count, pref_conv, pref_start,
                                     pref_end);
      }

      for (int i = 0; i < pref_count; i++) {
         if (minimal) {
            strbuf_appendf(&sb, "- %s: %s\n", prefs[i].category, prefs[i].value);
         } else {
            strbuf_appendf(&sb, "- %s: %s (reinforced %d times)\n", prefs[i].category,
                           prefs[i].value, prefs[i].reinforcement_count);
            if (with_source && source_budget > 0 && pref_conv[i] > 0) {
               append_source_excerpt_from_range(user_id, pref_conv[i], pref_start[i], pref_end[i],
                                                keywords, prefs[i].value, &sb, &source_budget,
                                                &seen);
               if (strbuf_oom(&sb))
                  break;
            }
         }
      }
   }

   /* Search summaries */
   memory_summary_t summaries[5];
   int summary_count = 0;

   if (token_count <= 1) {
      if (since_ts > 0)
         memory_db_summary_search_since(user_id, keywords, since_ts, summaries, 5, &summary_count);
      else
         memory_db_summary_search(user_id, keywords, summaries, 5, &summary_count);
   } else {
      /* Multi-word: search per token, dedup by ID */
      int64_t seen_sum_ids[20];
      int seen_sum_count = 0;

      for (int t = 0; t < token_count && summary_count < 5; t++) {
         memory_summary_t token_results[5];
         int n = 0;
         if (since_ts > 0)
            memory_db_summary_search_since(user_id, tokens[t], since_ts, token_results, 5, &n);
         else
            memory_db_summary_search(user_id, tokens[t], token_results, 5, &n);
         for (int j = 0; j < n && summary_count < 5; j++) {
            bool dup = false;
            for (int k = 0; k < seen_sum_count; k++) {
               if (seen_sum_ids[k] == token_results[j].id) {
                  dup = true;
                  break;
               }
            }
            if (!dup) {
               if (seen_sum_count < 20) {
                  seen_sum_ids[seen_sum_count++] = token_results[j].id;
               }
               summaries[summary_count++] = token_results[j];
            }
         }
      }
   }

   if (summary_count > 0) {
      if (strbuf_len(&sb) > 0)
         strbuf_append(&sb, "\n");
      if (minimal) {
         strbuf_append(&sb, "Conversation summaries:\n");
      } else {
         strbuf_appendf(&sb, "CONVERSATION SUMMARIES (%d):\n", summary_count);
      }

      int64_t sum_conv[5] = { 0 }, sum_start[5] = { 0 }, sum_end[5] = { 0 };
      if (with_source && !minimal) {
         int64_t sum_ids[5];
         for (int i = 0; i < summary_count; i++)
            sum_ids[i] = summaries[i].id;
         memory_db_summaries_get_sources(user_id, sum_ids, summary_count, sum_conv, sum_start,
                                         sum_end);
      }

      for (int i = 0; i < summary_count; i++) {
         if (minimal) {
            /* Inline topics, drop time prefix (extraction's leading-date convention
             * means dates are usually in the summary text itself). */
            strbuf_appendf(&sb, "- %s (Topics: %s)\n", summaries[i].summary, summaries[i].topics);
         } else {
            char time_str[32];
            format_time_ago(summaries[i].created_at, time_str, sizeof(time_str));
            strbuf_appendf(&sb, "- [%s] %s\n  Topics: %s\n", time_str, summaries[i].summary,
                           summaries[i].topics);
            if (with_source && source_budget > 0 && sum_conv[i] > 0) {
               append_source_excerpt_from_range(user_id, sum_conv[i], sum_start[i], sum_end[i],
                                                keywords, summaries[i].summary, &sb, &source_budget,
                                                &seen);
               if (strbuf_oom(&sb))
                  break;
            }
         }
      }
   }

   /* Append entity graph context (with relation source-rendering when enabled).
    * Skipped in minimal mode — graph context is a major contributor to the
    * verbose-format token count and the v5 hypothesis is that gpt-4o-mini
    * answers better with less context. */
   if (!minimal) {
      append_graph_context(user_id, keywords, as_of_ts, include_historical, &sb, with_source,
                           &source_budget, &seen);
   }

   /* Minimal mode: deferred global Source evidence block.  Emit one excerpt
    * per captured fact provenance (top 3 facts).  Uses the same
    * append_source_excerpt_from_range helper so dedup, top-K selection, and
    * formatting stay consistent with the verbose path. */
   if (minimal && with_source && min_src_count > 0 && source_budget > 0) {
      if (strbuf_len(&sb) > 0)
         strbuf_append(&sb, "\n");
      strbuf_append(&sb, "Source evidence:\n");
      for (int i = 0; i < min_src_count; i++) {
         if (source_budget <= 0)
            break;
         append_source_excerpt_from_range(user_id, min_src_conv[i], min_src_start[i],
                                          min_src_end[i], keywords, min_src_fact_text[i], &sb,
                                          &source_budget, &seen);
         if (strbuf_oom(&sb))
            break;
      }
   }

   /* Empty result: surface a friendly fallback (must not be NULL — callers
    * expect a heap-allocated string). */
   if (strbuf_len(&sb) == 0) {
      strbuf_appendf(&sb, "No memories found matching '%s'.", keywords);
   }

   /* OOM during build: return a sentinel; consumer (LLM) sees the failure. */
   if (strbuf_oom(&sb)) {
      strbuf_free(&sb);
      return strdup("Memory search failed: response too large or out of memory.");
   }

   char *out = strbuf_steal(&sb);
   return out ? out : strdup("Memory search failed: out of memory.");
}

/* =============================================================================
 * Action: Remember
 * ============================================================================= */

/* Lower cosine bound for the write-time 'remember' dedup advisory.  Below this,
 * facts are unrelated; the band's upper bound is paraphrase_dedup_threshold (the
 * extraction auto-merge cutoff), so this flags only the uncertain middle. */
#define MEMORY_REMEMBER_BAND_LOW 0.80f

/* Max facts accepted in one batched 'remember' call (JSON-array form).  Bounds the
 * per-call work (each fact runs the full dedup + embed + advisory pipeline) and the
 * aggregate response size. */
#define MAX_REMEMBER_FACTS 25

/* Initial strbuf capacity for the batch-remember advisory: ~1 advisory line per fact
 * (~160 chars) across the MAX_REMEMBER_FACTS bound, so a full batch usually avoids any
 * realloc; strbuf still grows on overflow. */
#define MEMORY_REMEMBER_BATCH_BUF_INIT 4096

/* Store ONE fact: injection check → multi-stage dedup → store → embed → similarity
 * advisory.  Returns a malloc'd, caller-freed result line.  The batch dispatcher
 * (memory_action_remember) calls this once per fact; single-fact callers reach it
 * through that dispatcher's non-array fall-through, so behavior is unchanged. */
/* expires_ref: AI-decided expiry reference date (unix sec) for a transient dated
 * fact, or 0 for durable.  When set and [memory] expire_enabled is on, a fresh
 * fact gets expires_at = expires_ref + grace (v58/C3).  The AI is the judge — we
 * never derive this; it only fires when the model attaches a date. */
static char *memory_action_remember_single(int user_id,
                                           const char *fact_text,
                                           int64_t expires_ref) {
   if (!fact_text || strlen(fact_text) == 0) {
      return strdup("Please provide the fact to remember.");
   }

   if (strlen(fact_text) > MEMORY_FACT_TEXT_MAX - 1) {
      /* Report the actual limit and tell the model how to recover — split the content
       * into multiple shorter 'remember' calls (one fact each), not one oversized fact.
       * Stack buffer (not static): memory tool callbacks run on parallel worker threads. */
      char too_long_msg[192];
      snprintf(too_long_msg, sizeof(too_long_msg),
               "The fact is too long (%zu chars; max %d). Split it into several shorter "
               "'remember' calls — one concise fact each — rather than one big entry.",
               strlen(fact_text), MEMORY_FACT_TEXT_MAX - 1);
      return strdup(too_long_msg);
   }

   /* Check for injection patterns */
   if (memory_filter_check(fact_text)) {
      return strdup("I cannot store that as a fact. It contains patterns that could affect my "
                    "behavior in unintended ways.");
   }

   /* Stage 1: Fast hash-based duplicate check */
   uint32_t fact_hash = memory_normalize_and_hash(fact_text);

   if (fact_hash != 0) {
      memory_fact_t hash_matches[5];
      int hash_count = 0;
      memory_db_fact_find_by_hash(user_id, fact_hash, hash_matches, 5, &hash_count);

      if (hash_count > 0) {
         /* Verify with Jaccard similarity (handles hash collisions) */
         for (int i = 0; i < hash_count; i++) {
            if (memory_is_duplicate(fact_text, hash_matches[i].fact_text,
                                    MEMORY_SIMILARITY_THRESHOLD)) {
               /* Exact or near-exact duplicate - reinforce confidence */
               float new_conf = hash_matches[i].confidence + 0.1f;
               if (new_conf > 1.0f)
                  new_conf = 1.0f;
               memory_db_fact_update_confidence(hash_matches[i].id, user_id, new_conf);
               OLOG_INFO("memory_callback: duplicate detected (hash match), reinforced fact %ld",
                         (long)hash_matches[i].id);
               return strdup("I already know that. Increased my confidence in this fact.");
            }
         }
      }
   }

   /* Stage 2: SQL LIKE search for potential fuzzy duplicates */
   memory_fact_t similar[5];
   int similar_count = 0;
   memory_db_fact_find_similar(user_id, fact_text, similar, 5, &similar_count);

   if (similar_count > 0) {
      /* Check Jaccard similarity on candidates */
      for (int i = 0; i < similar_count; i++) {
         float similarity = memory_jaccard_similarity(fact_text, similar[i].fact_text);
         if (similarity >= MEMORY_SIMILARITY_THRESHOLD) {
            /* Similar enough to be considered a duplicate */
            float new_conf = similar[i].confidence + 0.1f;
            if (new_conf > 1.0f)
               new_conf = 1.0f;
            memory_db_fact_update_confidence(similar[i].id, user_id, new_conf);
            OLOG_INFO("memory_callback: duplicate detected (Jaccard=%.2f), reinforced fact %ld",
                      similarity, (long)similar[i].id);
            return strdup(
                "I already know something similar. Increased my confidence in that fact.");
         }
      }
   }

   /* Stage 3: Semantic band lookup for a write-time dedup advisory.  Lexical
    * stages above catch exact/near-exact paraphrases; this surfaces facts that
    * are SEMANTICALLY close but lexically distinct (cosine in [low, high)) so the
    * model can choose to 'forget' any the new fact supersedes.  The band's upper
    * bound is the extraction paraphrase-dedup threshold: matches >= that get
    * auto-merged by extraction anyway, so we only flag the uncertain middle band.
    * Computed BEFORE the new fact is stored, so it never self-matches. */
   float query_vec[MAX_EMBEDDING_DIMS];
   int query_dims = 0;
   bool have_vec = false;
   int64_t neighbor_ids[MEMORY_BAND_NEIGHBORS_MAX];
   float neighbor_scores[MEMORY_BAND_NEIGHBORS_MAX];
   int neighbor_count = 0;
   if (memory_embeddings_available() &&
       memory_embeddings_embed(fact_text, query_vec, &query_dims) == 0 && query_dims > 0) {
      have_vec = true;
      float high = g_config.memory.paraphrase_dedup_threshold;
      if (high <= 0.0f || high > 1.0f)
         high = MEMORY_PARAPHRASE_DEDUP_DEFAULT;
      /* Failure (cache load / dim mismatch) degrades to no advisory — neighbor_count
       * stays 0, so the cast-away return is intentional, not an unchecked call. */
      (void)memory_embeddings_band_neighbors(user_id, query_vec, query_dims,
                                             MEMORY_REMEMBER_BAND_LOW, high, neighbor_ids,
                                             neighbor_scores, MEMORY_BAND_NEIGHBORS_MAX,
                                             &neighbor_count);
   }

   /* No duplicates found - store the new fact (no provenance: user-initiated) */
   int64_t fact_id = 0;
   int create_rc = memory_db_fact_create(user_id, fact_text, 1.0f, "explicit", NULL, NULL,
                                         &fact_id);

   if (create_rc != MEMORY_DB_SUCCESS) {
      return strdup("Failed to store the fact. Please try again.");
   }

   /* AI-decided expiry (v58/C3): if the model attached a reference date and
    * expiry is enabled, set expires_at = ref + grace so this transient fact
    * leaves retrieval after its date.  Durable facts (expires_ref == 0) are
    * untouched. */
   if (g_config.memory.expire_enabled && expires_ref > 0) {
      int64_t cutoff = expires_ref + (int64_t)g_config.memory.expire_grace_days * 86400;
      memory_db_fact_set_expires_at(fact_id, user_id, cutoff);
      OLOG_INFO("memory_callback: remember set expires_at=%ld (ref+grace=%dd) on fact %lld",
                (long)cutoff, g_config.memory.expire_grace_days, (long long)fact_id);
   }

   /* Store the embedding we already computed (avoids a second embed call) when
    * available; otherwise fall back to the embed-and-store convenience path. */
   if (have_vec) {
      memory_embeddings_store_precomputed(user_id, fact_id, query_vec, query_dims);
   } else if (memory_embeddings_available()) {
      memory_embeddings_embed_and_store(user_id, fact_id, fact_text);
   }

   /* Build the response: confirmation line + (only when present) an advisory
    * block listing the semantically-similar existing facts the model may want to
    * supersede.  Advisory only — 'forget' is a hard delete, so never auto-act. */
   strbuf_t sb;
   strbuf_init(&sb, 256);
   strbuf_appendf(&sb, "Remembered: \"%s\" (ID:%lld)", fact_text, (long long)fact_id);

   if (neighbor_count > 0) {
      strbuf_appendf(&sb,
                     "\n\nSimilar facts already in memory — if this fact fully replaces any of "
                     "them, 'forget' that ID (comma-separate to remove several). Leave the rest:");
      /* The embedding cache holds only ids/embeddings/norms, not fact_text, so the
       * text must be fetched per neighbor (bounded by MEMORY_BAND_NEIGHBORS_MAX).
       * Deliberate — don't "fix" by widening the cache with text it doesn't need. */
      for (int i = 0; i < neighbor_count; i++) {
         memory_fact_t nf;
         if (memory_db_fact_get(neighbor_ids[i], user_id, &nf) != MEMORY_DB_SUCCESS)
            continue; /* vanished between scan and render — skip */
         strbuf_appendf(&sb, "\n  - [ID:%lld] %.200s (sim %.2f)", (long long)neighbor_ids[i],
                        nf.fact_text, neighbor_scores[i]);
      }
   }

   char *result = strbuf_steal(&sb);
   if (!result) {
      strbuf_free(&sb);
      result = strdup("Fact stored successfully.");
   }
   return result;
}

/* 'remember' entry point.  Accepts either a single fact string (the common case,
 * unchanged) or a JSON array for a one-round-trip batch dump.  Each array element is
 * EITHER a plain string (durable fact) OR an object {"text": "...", "expires_at":
 * "YYYY-MM-DD"} where expires_at marks a transient dated fact the AI judged should
 * expire after that date (v58/C3).  Mixed forms are allowed, e.g.
 * ["Jon prefers dark mode", {"text": "Movie tonight 6:30 PM", "expires_at":
 * "2026-06-05"}].  Each element runs the full single-fact pipeline (dedup + embed +
 * advisory); results are concatenated.  A value that is not a JSON array (parse
 * failure or any non-array type) falls through to the single-fact path, so existing
 * callers — and any ordinary fact text — are unaffected.  Capped at
 * MAX_REMEMBER_FACTS per call. */
static char *memory_action_remember(int user_id, const char *value) {
   if (!value || !*value) {
      return strdup("Please provide the fact to remember.");
   }

   struct json_object *arr = json_tokener_parse(value);
   if (arr && json_object_is_type(arr, json_type_array)) {
      int n = json_object_array_length(arr);
      strbuf_t sb;
      strbuf_init(&sb, MEMORY_REMEMBER_BATCH_BUF_INIT);
      int stored = 0;
      int processed = 0;
      for (int i = 0; i < n && processed < MAX_REMEMBER_FACTS; i++) {
         struct json_object *elem = json_object_array_get_idx(arr, i);
         /* Element may be a plain string (durable) or {text, expires_at} (transient). */
         const char *fact = NULL;
         int64_t expires_ref = 0;
         if (elem && json_object_is_type(elem, json_type_string)) {
            fact = json_object_get_string(elem);
         } else if (elem && json_object_is_type(elem, json_type_object)) {
            struct json_object *text_obj, *exp_obj;
            if (json_object_object_get_ex(elem, "text", &text_obj)) {
               fact = json_object_get_string(text_obj);
            }
            if (json_object_object_get_ex(elem, "expires_at", &exp_obj)) {
               const char *exp_str = json_object_get_string(exp_obj);
               if (exp_str && *exp_str) {
                  expires_ref = iso8601_parse_date_utc(exp_str);
               }
            }
         }
         if (!fact || !*fact) {
            continue; /* skip empty / unrecognized elements */
         }
         char *one = memory_action_remember_single(user_id, fact, expires_ref);
         if (one) {
            if (stored > 0) {
               strbuf_append(&sb, "\n\n");
            }
            strbuf_append(&sb, one);
            free(one);
            one = NULL;
            stored++;
         }
         processed++;
      }
      if (n > MAX_REMEMBER_FACTS) {
         strbuf_appendf(&sb,
                        "\n\n(Stored the first %d facts; %d more were not processed — send the "
                        "rest in another 'remember' call.)",
                        MAX_REMEMBER_FACTS, n - MAX_REMEMBER_FACTS);
      }
      json_object_put(arr);
      if (stored == 0) {
         strbuf_free(&sb);
         return strdup("No valid facts found in the list to remember.");
      }
      char *result = strbuf_steal(&sb);
      if (!result) {
         strbuf_free(&sb);
         return strdup("Stored the facts successfully.");
      }
      return result;
   }
   if (arr) {
      json_object_put(arr);
   }

   /* Single fact (backward-compatible default).  No expiry on the bare-string
    * form — transient facts use the array-of-objects form above. */
   return memory_action_remember_single(user_id, value, 0);
}

/* =============================================================================
 * Action: Forget
 * ============================================================================= */

/* Max fact IDs accepted in one bulk 'forget' call — bounds the response size. */
#define MAX_FORGET_IDS 50

/* Parse one or more comma/space-separated POSITIVE numeric fact IDs into `out`
 * (capacity `max`).  Numeric-only tokens are required (guards against accidental
 * fuzzy-text operations); non-numeric / non-positive tokens are counted in
 * *invalid_out and skipped.  *capped_out is set when more than `max` IDs were
 * present.  Returns the number of IDs written.  Shared by 'forget' and 'get'. */
static int parse_id_list(const char *s, int64_t *out, int max, int *invalid_out, bool *capped_out) {
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

static char *memory_action_forget(int user_id, const char *fact_text, int64_t replaced_by) {
   if (!fact_text || strlen(fact_text) == 0) {
      return strdup("Please specify the fact ID(s) to forget (e.g. '42' or '42,57,91'). Use "
                    "'search', 'recent', or 'find_duplicates' first to find the IDs.");
   }

   int64_t ids[MAX_FORGET_IDS];
   int invalid_count = 0;
   bool capped = false;
   int id_count = parse_id_list(fact_text, ids, MAX_FORGET_IDS, &invalid_count, &capped);

   if (id_count == 0) {
      return strdup("Please provide numeric fact ID(s) (e.g. '42' or '42,57,91'). Use 'search', "
                    "'recent', or 'find_duplicates' first to find the IDs.");
   }

   /* Merge mode (replaced_by set) supersedes each ID into the keeper instead of
    * hard-deleting: the listed facts are hidden from recall but kept recoverable, pointing
    * at the survivor (provenance preserved).  Validate the keeper up front — it must exist,
    * be owned, and not be one of the IDs being merged into it. */
   const bool merge = (replaced_by > 0);
   if (merge) {
      memory_fact_t keep;
      if (memory_db_fact_get(replaced_by, user_id, &keep) != MEMORY_DB_SUCCESS) {
         return strdup("The 'replaced_by' keeper ID wasn't found (or isn't yours). "
                       "Nothing was changed.");
      }
      for (int i = 0; i < id_count; i++) {
         if (ids[i] == replaced_by) {
            return strdup("'replaced_by' must be the fact you are KEEPING, not one of the IDs "
                          "being merged into it. Nothing was changed.");
         }
      }
   }

   /* Apply to each, tracking outcomes.  fact_get verifies existence + ownership
    * (SQL filters by user_id — wrong-user lookups return NOT_FOUND, no oracle). */
   int forgotten = 0;
   int not_removed = 0;
   char first_text[256] = "";
   bool first_truncated = false;
   char ok_ids[600] = "";
   char nf_ids[400] = "";
   size_t ok_pos = 0, nf_pos = 0;

   for (int i = 0; i < id_count; i++) {
      memory_fact_t fact;
      bool removed = false;
      if (memory_db_fact_get(ids[i], user_id, &fact) == MEMORY_DB_SUCCESS &&
          (merge ? memory_db_fact_supersede(ids[i], replaced_by, user_id)
                 : memory_db_fact_delete(ids[i], user_id)) == MEMORY_DB_SUCCESS) {
         removed = true;
         if (forgotten == 0) {
            snprintf(first_text, sizeof(first_text), "%.200s", fact.fact_text);
            first_truncated = strlen(fact.fact_text) > 200;
         }
         forgotten++;
      }
      char *list = removed ? ok_ids : nf_ids;
      size_t *lp = removed ? &ok_pos : &nf_pos;
      size_t cap = removed ? sizeof(ok_ids) : sizeof(nf_ids);
      if (!removed)
         not_removed++;
      if (*lp < cap - 24)
         *lp += (size_t)snprintf(list + *lp, cap - *lp, "%s%lld", *lp ? ", " : "",
                                 (long long)ids[i]);
   }

   /* Single valid ID that succeeded → detailed response (back-compat). */
   if (id_count == 1 && forgotten == 1) {
      char *msg = malloc(384);
      if (msg) {
         if (merge)
            snprintf(msg, 384,
                     "Merged ID %lld into ID %lld (hidden from recall, recoverable): \"%s%s\"",
                     (long long)ids[0], (long long)replaced_by, first_text,
                     first_truncated ? "..." : "");
         else
            snprintf(msg, 384, "Forgotten (ID %lld): \"%s%s\"", (long long)ids[0], first_text,
                     first_truncated ? "..." : "");
      }
      return msg ? msg
                 : strdup(merge ? "Fact merged successfully." : "Fact forgotten successfully.");
   }

   /* Batched summary.  Clamp `pos` after each append so the `SZ - pos` size
    * argument can never underflow to a huge size_t if the ok_ids/nf_ids caps
    * are later enlarged (defense-in-depth — today's worst case is ~1099). */
   enum {
      FORGET_MSG_SZ = 1200
   };
   char *msg = malloc(FORGET_MSG_SZ);
   if (!msg)
      return strdup("Memory updated.");
   int pos;
   if (merge)
      pos = snprintf(msg, FORGET_MSG_SZ, "Merged %d fact%s into ID %lld", forgotten,
                     forgotten == 1 ? "" : "s", (long long)replaced_by);
   else
      pos = snprintf(msg, FORGET_MSG_SZ, "Forgotten %d fact%s", forgotten,
                     forgotten == 1 ? "" : "s");
   if (pos < 0 || pos >= FORGET_MSG_SZ)
      pos = FORGET_MSG_SZ - 1;
   if (forgotten > 0) {
      pos += snprintf(msg + pos, FORGET_MSG_SZ - pos,
                      merge ? " (IDs %s — hidden from recall, recoverable)" : " (IDs %s)", ok_ids);
      if (pos >= FORGET_MSG_SZ)
         pos = FORGET_MSG_SZ - 1;
   }
   if (not_removed > 0) {
      pos += snprintf(msg + pos, FORGET_MSG_SZ - pos, "; %d not found (%s)", not_removed, nf_ids);
      if (pos >= FORGET_MSG_SZ)
         pos = FORGET_MSG_SZ - 1;
   }
   if (invalid_count > 0) {
      pos += snprintf(msg + pos, FORGET_MSG_SZ - pos, "; ignored %d non-numeric token%s",
                      invalid_count, invalid_count == 1 ? "" : "s");
      if (pos >= FORGET_MSG_SZ)
         pos = FORGET_MSG_SZ - 1;
   }
   if (capped)
      snprintf(msg + pos, FORGET_MSG_SZ - pos, " (capped at %d IDs per call)", MAX_FORGET_IDS);
   return msg;
}

/* =============================================================================
 * Action: Get (deterministic retrieval by ID)
 *
 * The exact-fetch counterpart to 'search': returns specific facts verbatim by
 * numeric ID, bypassing relevance ranking entirely.  Use when the ID(s) are
 * already known (from a prior search/recent/find_duplicates result) and the
 * exact stored text is wanted — search is semantic and can bury an exact
 * record under its near-twins.  Shares the 'forget' ID parser (positive,
 * comma/space-separated, non-numeric tokens ignored, capped at MAX_FORGET_IDS).
 * ============================================================================= */
static char *memory_action_get(int user_id, const char *value) {
   if (!value || !value[0]) {
      return strdup("Please provide one or more numeric memory IDs to retrieve (e.g. '7858' or "
                    "'7858,7854'). IDs come from a prior search/recent/find_duplicates result.");
   }

   int64_t ids[MAX_FORGET_IDS];
   int invalid_count = 0;
   bool capped = false;
   int id_count = parse_id_list(value, ids, MAX_FORGET_IDS, &invalid_count, &capped);

   if (id_count == 0) {
      return strdup("Please provide numeric memory ID(s) (e.g. '7858' or '7858,7854').");
   }

   strbuf_t sb;
   strbuf_init(&sb, 1024);
   strbuf_append(&sb, "Memories:\n");

   int found = 0;
   char nf_ids[400] = "";
   size_t nf_len = 0;
   for (int i = 0; i < id_count; i++) {
      memory_fact_t fact;
      /* memory_db_fact_get is user-scoped at the SQL layer, so a foreign or
       * absent ID simply misses — no cross-user leak. */
      if (memory_db_fact_get(ids[i], user_id, &fact) == MEMORY_DB_SUCCESS) {
         strbuf_appendf(&sb, "- [ID:%lld] %s\n", (long long)ids[i], fact.fact_text);
         found++;
      } else if (nf_len < sizeof(nf_ids) - 1) {
         nf_len += (size_t)snprintf(nf_ids + nf_len, sizeof(nf_ids) - nf_len, "%s%lld",
                                    nf_len ? ", " : "", (long long)ids[i]);
         if (nf_len >= sizeof(nf_ids))
            nf_len = sizeof(nf_ids) - 1;
      }
   }

   if (found == 0) {
      strbuf_free(&sb);
      char msg[480];
      snprintf(msg, sizeof(msg),
               "No memories found for ID(s): %s. They may have been forgotten or aren't yours.",
               nf_ids);
      return strdup(msg);
   }

   if (nf_ids[0])
      strbuf_appendf(&sb, "(not found: %s)\n", nf_ids);
   if (capped)
      strbuf_appendf(&sb, "(capped at %d IDs per call)\n", MAX_FORGET_IDS);

   if (strbuf_oom(&sb)) {
      strbuf_free(&sb);
      return strdup("Memory query failed: response too large or out of memory.");
   }
   char *out = strbuf_steal(&sb);
   return out ? out : strdup("Memory query failed: out of memory.");
}

/* =============================================================================
 * Action: Find Duplicates
 *
 * Surfaces clusters of near-duplicate facts (by embedding cosine) with their IDs
 * so the user/assistant can review and 'forget' the redundant ones.  Optional
 * `query` = similarity threshold (0.50-1.00, default 0.85 — below the 0.92
 * paraphrase-dedup gate, to catch near-dups that escaped it at extraction).
 * ============================================================================= */
static char *memory_action_find_duplicates(int user_id, const char *value) {
   float threshold = 0.85f;
   if (value && *value) {
      char *endptr = NULL;
      float t = strtof(value, &endptr);
      if (endptr != value && t >= 0.5f && t <= 1.0f)
         threshold = t;
   }

   memory_dup_cluster_t clusters[MEMORY_DUP_MAX_CLUSTERS];
   int cluster_count = 0;
   if (memory_embeddings_find_duplicate_clusters(user_id, threshold, clusters,
                                                 MEMORY_DUP_MAX_CLUSTERS,
                                                 &cluster_count) != MEMORY_DB_SUCCESS) {
      return strdup("Couldn't scan memories for duplicates (embeddings may be unavailable).");
   }

   if (cluster_count == 0) {
      char *msg = malloc(160);
      if (msg)
         snprintf(msg, 160,
                  "No near-duplicate fact clusters found (similarity >= %.2f). Memory looks clean.",
                  (double)threshold);
      return msg ? msg : strdup("No near-duplicate fact clusters found.");
   }

   strbuf_t sb;
   strbuf_init(&sb, 4096);
   strbuf_appendf(
       &sb,
       "Found %d near-duplicate cluster%s (similarity >= %.2f). For each cluster: if the "
       "facts are the SAME fact restated, keep the best one and 'forget' the redundant "
       "IDs with 'replaced_by' set to the keeper's ID (this merges them — hidden from "
       "recall but recoverable). Facts that only share a TOPIC but differ — weather on "
       "different dates, events on different days, an item requested vs. received — are "
       "NOT duplicates; keep them all.\n",
       cluster_count, cluster_count == 1 ? "" : "s", (double)threshold);
   for (int c = 0; c < cluster_count; c++) {
      strbuf_appendf(&sb, "\nCluster %d (%d facts, min similarity %.2f):\n", c + 1,
                     clusters[c].count, (double)clusters[c].min_similarity);
      for (int k = 0; k < clusters[c].count; k++) {
         memory_fact_t fact;
         /* Skip NOT_FOUND (a just-deleted fact / cache-vs-DB race). */
         if (memory_db_fact_get(clusters[c].ids[k], user_id, &fact) == MEMORY_DB_SUCCESS) {
            strbuf_appendf(&sb, "  - [ID:%lld] %.160s%s\n", (long long)clusters[c].ids[k],
                           fact.fact_text, strlen(fact.fact_text) > 160 ? "..." : "");
         }
      }
   }

   if (strbuf_oom(&sb)) {
      strbuf_free(&sb);
      return strdup("Duplicate scan succeeded but the result was too large to render.");
   }
   char *out = strbuf_steal(&sb);
   return out ? out : strdup("Memory updated.");
}

/* =============================================================================
 * Action: Recent
 *
 * Returns facts and summaries created within a specified time period.
 * ============================================================================= */

/* MEMORY_RECENT_LIMIT_MAX + defaults moved to memory_callback_internal.h so
 * the tool schema description and the action layer share one source of
 * truth — see header for rationale. */
_Static_assert(MEMORY_RECENT_LIMIT_MAX >= MEMORY_RECENT_DEFAULT_FACT_LIMIT &&
                   MEMORY_RECENT_LIMIT_MAX >= MEMORY_RECENT_DEFAULT_SUMMARY_LIMIT,
               "MEMORY_RECENT_LIMIT_MAX must be >= the defaults used to size stack arrays");

char *memory_action_recent(int user_id,
                           const char *period,
                           const char *before,
                           bool sort_asc,
                           int limit,
                           bool with_source,
                           int source_budget) {
   /* Resolve lower bound — empty/NULL period defaults to "7d" so the LLM
    * can omit the time window for the common "show me recent stuff" case
    * (pre-Bundle-3 required a non-empty period and rejected with an
    * error message). */
   const char *period_resolved = (period && period[0]) ? period : "7d";
   time_t seconds = parse_time_period(period_resolved);
   if (seconds <= 0) {
      return strdup("Invalid time period. Use format like '24h', '7d', '1w', or '30m'.");
   }
   time_t now_ts = time(NULL);
   time_t since = now_ts - seconds;
   if (since < 0)
      since = 0;

   /* Resolve upper bound — empty/NULL `before` means "until now"; pass 0
    * to memory_db_*_list_window which resolves to INT64_MAX internally. */
   time_t until = 0;
   if (before && before[0]) {
      time_t before_seconds = parse_time_period(before);
      if (before_seconds <= 0) {
         return strdup("Invalid 'before' time period. Use format like '3d', '30d', '1w'.");
      }
      until = now_ts - before_seconds;
      if (until < 0)
         until = 0;
   }

   /* Clamp limit; 0 = "use defaults". */
   int fact_limit = MEMORY_RECENT_DEFAULT_FACT_LIMIT;
   int summary_limit = MEMORY_RECENT_DEFAULT_SUMMARY_LIMIT;
   if (limit > 0) {
      if (limit > MEMORY_RECENT_LIMIT_MAX)
         limit = MEMORY_RECENT_LIMIT_MAX;
      fact_limit = limit;
      summary_limit = limit;
   }

   /* Growable response buffer — see memory_action_search() for rationale. */
   strbuf_t sb;
   strbuf_init(&sb, 4096);

   /* Get facts within the window, sorted per sort_asc.
    * Stack footprint: sizeof(memory_fact_t) ≈ 612 B × MEMORY_RECENT_LIMIT_MAX
    * (50) ≈ 30 KB.  Plus three int64_t[50] provenance arrays below = ~1.2 KB.
    * Total fact-side stack ≈ 31 KB.  Safe on glibc's 8 MB default pthread
    * stack; if MEMORY_RECENT_LIMIT_MAX ever bumps past ~256 (≈150 KB), move
    * these to the heap. */
   memory_fact_t facts[MEMORY_RECENT_LIMIT_MAX];
   int fact_count = 0;
   memory_db_fact_list_window(user_id, since, until, sort_asc, facts, fact_limit, &fact_count);

   /* See memory_action_search for source_budget clamp rationale. */
   if (source_budget <= 0)
      source_budget = MEMORY_SOURCE_BUDGET_CHARS;
   else if (source_budget > MEMORY_SOURCE_BUDGET_MAX)
      source_budget = MEMORY_SOURCE_BUDGET_MAX;

   /* Dedup spans facts + summaries: same range cited by a fact and a summary
    * emits "same as above" after the first verbatim. */
   source_dedup_set_t seen = { 0 };

   /* Minimal mode parity with memory_action_search.  See that function's
    * declaration block for design rationale. */
   const bool minimal = g_config.memory.memory_text_minimal;
   enum {
      MINIMAL_RECENT_SOURCE_TOP_FACTS = 3
   };
   int64_t min_src_conv[MINIMAL_RECENT_SOURCE_TOP_FACTS] = { 0 };
   int64_t min_src_start[MINIMAL_RECENT_SOURCE_TOP_FACTS] = { 0 };
   int64_t min_src_end[MINIMAL_RECENT_SOURCE_TOP_FACTS] = { 0 };
   const char *min_src_fact_text[MINIMAL_RECENT_SOURCE_TOP_FACTS] = { NULL };
   int min_src_count = 0;

   if (fact_count > 0) {
      if (minimal) {
         strbuf_append(&sb, sort_asc ? "Oldest memories:\n" : "Recent memories:\n");
      } else {
         strbuf_append(&sb, sort_asc ? "OLDEST FACTS:\n" : "RECENT FACTS:\n");
      }

      /* Batch-fetch fact provenance — memory_db_facts_get_sources chunks
       * internally in MAX_PROVENANCE_BATCH-sized (32) groups, so N > 32 is
       * fine.  fact_count is always ≤ fact_limit ≤ MEMORY_RECENT_LIMIT_MAX
       * (50), worst case 2 chunks. */
      int64_t fact_conv[MEMORY_RECENT_LIMIT_MAX] = { 0 };
      int64_t fact_start[MEMORY_RECENT_LIMIT_MAX] = { 0 };
      int64_t fact_end[MEMORY_RECENT_LIMIT_MAX] = { 0 };
      if (with_source) {
         int64_t fact_ids[MEMORY_RECENT_LIMIT_MAX];
         for (int i = 0; i < fact_count; i++)
            fact_ids[i] = facts[i].id;
         memory_db_facts_get_sources(user_id, fact_ids, fact_count, fact_conv, fact_start,
                                     fact_end);
      }

      int elided = 0;
      for (int i = 0; i < fact_count; i++) {
         if (minimal) {
            strbuf_appendf(&sb, "- [ID:%lld] %s\n", (long long)facts[i].id, facts[i].fact_text);
            if (with_source && fact_conv[i] > 0 &&
                min_src_count < MINIMAL_RECENT_SOURCE_TOP_FACTS) {
               min_src_conv[min_src_count] = fact_conv[i];
               min_src_start[min_src_count] = fact_start[i];
               min_src_end[min_src_count] = fact_end[i];
               min_src_fact_text[min_src_count] = facts[i].fact_text;
               min_src_count++;
            }
         } else {
            char time_str[32];
            format_time_ago(facts[i].created_at, time_str, sizeof(time_str));
            strbuf_appendf(&sb, "- [ID:%lld] %s (%s, %s)\n", (long long)facts[i].id,
                           facts[i].fact_text, facts[i].source, time_str);
            if (with_source && source_budget > 0 && fact_conv[i] > 0) {
               /* memory_action_recent is time-windowed, not query-driven — pass
                * NULL for the query scoring slot and use the fact_text alone
                * to rank dialog messages within the provenance range. */
               append_source_excerpt_from_range(user_id, fact_conv[i], fact_start[i], fact_end[i],
                                                NULL, facts[i].fact_text, &sb, &source_budget,
                                                &seen);
            } else if (with_source && source_budget <= 0) {
               elided++;
            }
         }
         /* See memory_action_search for OOM-bail rationale (embedded M4). */
         if (strbuf_oom(&sb))
            break;
      }
      if (elided > 0) {
         strbuf_appendf(&sb, "[%d more source(s) elided — call context_expand for full]\n", elided);
      }
   }

   /* Get summaries within the window, sorted per sort_asc.
    * Stack footprint: sizeof(memory_summary_t) ≈ 2420 B (dominated by the
    * 2048-byte `summary` field) × MEMORY_RECENT_LIMIT_MAX (50) ≈ 121 KB.
    * This is the bulk of the function's stack budget — combined with the
    * fact arrays above the total reaches ~154 KB.  Safe on glibc's 8 MB
    * default pthread stack but bumping MEMORY_RECENT_LIMIT_MAX past ~256
    * would push past 600 KB — move these to the heap if that bump lands. */
   memory_summary_t summaries[MEMORY_RECENT_LIMIT_MAX];
   int summary_count = 0;
   memory_db_summary_list_window(user_id, since, until, sort_asc, summaries, summary_limit,
                                 &summary_count);

   if (summary_count > 0) {
      if (strbuf_len(&sb) > 0)
         strbuf_append(&sb, "\n");
      if (minimal) {
         strbuf_append(&sb, sort_asc ? "Oldest conversations:\n" : "Recent conversations:\n");
      } else {
         strbuf_append(&sb, sort_asc ? "OLDEST CONVERSATIONS:\n" : "RECENT CONVERSATIONS:\n");
      }

      int64_t sum_conv[MEMORY_RECENT_LIMIT_MAX] = { 0 };
      int64_t sum_start[MEMORY_RECENT_LIMIT_MAX] = { 0 };
      int64_t sum_end[MEMORY_RECENT_LIMIT_MAX] = { 0 };
      if (with_source && !minimal) {
         int64_t sum_ids[MEMORY_RECENT_LIMIT_MAX];
         for (int i = 0; i < summary_count; i++)
            sum_ids[i] = summaries[i].id;
         memory_db_summaries_get_sources(user_id, sum_ids, summary_count, sum_conv, sum_start,
                                         sum_end);
      }

      for (int i = 0; i < summary_count; i++) {
         if (minimal) {
            if (strbuf_appendf(&sb, "- %s (Topics: %s)\n", summaries[i].summary,
                               summaries[i].topics) < 0)
               break;
         } else {
            char time_str[32];
            format_time_ago(summaries[i].created_at, time_str, sizeof(time_str));
            if (strbuf_appendf(&sb, "- [%s] %s\n  Topics: %s\n", time_str, summaries[i].summary,
                               summaries[i].topics) < 0)
               break;
            if (with_source && source_budget > 0 && sum_conv[i] > 0) {
               /* Time-windowed (no query) — see fact branch above. */
               append_source_excerpt_from_range(user_id, sum_conv[i], sum_start[i], sum_end[i],
                                                NULL, summaries[i].summary, &sb, &source_budget,
                                                &seen);
            }
         }
         /* Mirror memory_action_search's OOM-bail pattern (Arch review M4). */
         if (strbuf_oom(&sb))
            break;
      }
   }

   /* Minimal mode: deferred global Source evidence block. */
   if (minimal && with_source && min_src_count > 0 && source_budget > 0) {
      if (strbuf_len(&sb) > 0)
         strbuf_append(&sb, "\n");
      strbuf_append(&sb, "Source evidence:\n");
      for (int i = 0; i < min_src_count; i++) {
         if (source_budget <= 0)
            break;
         /* Time-windowed: pass NULL keyword scoring (fact_text alone ranks). */
         append_source_excerpt_from_range(user_id, min_src_conv[i], min_src_start[i],
                                          min_src_end[i], NULL, min_src_fact_text[i], &sb,
                                          &source_budget, &seen);
         if (strbuf_oom(&sb))
            break;
      }
   }

   if (fact_count == 0 && summary_count == 0) {
      strbuf_appendf(&sb, "No memories found in the past %s.", period_resolved);
   } else {
      strbuf_appendf(&sb, "\nTotal: %d facts, %d conversations", fact_count, summary_count);
   }

   if (strbuf_oom(&sb)) {
      strbuf_free(&sb);
      return strdup("Memory query failed: response too large or out of memory.");
   }

   char *out = strbuf_steal(&sb);
   return out ? out : strdup("Memory query failed: out of memory.");
}

/* =============================================================================
 * Action: Save Contact
 * ============================================================================= */

static char *memory_action_save_contact(int user_id, const char *value) {
   /* value format: "entity_name::field_type::type::value::val::label::lbl" */
   if (!value || !value[0])
      return strdup("Error: save_contact requires entity name, field_type, and value");

   char entity_name[128] = "";
   tool_param_extract_base(value, entity_name, sizeof(entity_name));

   char field_type[32] = "";
   tool_param_extract_custom(value, "field_type", field_type, sizeof(field_type));

   char contact_value[256] = "";
   tool_param_extract_custom(value, "value", contact_value, sizeof(contact_value));

   char label[32] = "";
   tool_param_extract_custom(value, "label", label, sizeof(label));

   /* Optional entity_id for disambiguation (from a prior fuzzy match prompt) */
   char entity_id_str[32] = "";
   tool_param_extract_custom(value, "entity_id", entity_id_str, sizeof(entity_id_str));

   if (!entity_name[0] || !field_type[0] || !contact_value[0])
      return strdup("Error: save_contact requires entity name, field_type, and value");

   int64_t entity_id;

   if (entity_id_str[0]) {
      /* Explicit entity_id provided — use it directly (disambiguation resolved) */
      entity_id = atoll(entity_id_str);
      if (entity_id <= 0)
         return strdup("Error: invalid entity_id");
   } else {
      /* Check for exact canonical match first */
      char canonical[64];
      memory_make_canonical_name(entity_name, canonical, sizeof(canonical));

      memory_entity_t exact;
      int exact_rc = memory_db_entity_get_by_name(user_id, canonical, &exact);

      if (exact_rc == MEMORY_DB_SUCCESS) {
         /* Exact match — use existing entity */
         entity_id = exact.id;
      } else {
         /* No exact match — search for similar entities */
         memory_entity_t similar[5];
         int sim_count = 0;
         memory_db_entity_search(user_id, entity_name, similar, 5, &sim_count);

         /* Filter to person entities only */
         int person_count = 0;
         memory_entity_t persons[5];
         for (int i = 0; i < sim_count; i++) {
            if (strcmp(similar[i].entity_type, "person") == 0) {
               persons[person_count++] = similar[i];
            }
         }

         if (person_count > 0) {
            /* Similar people found — return disambiguation prompt */
            char *buf = malloc(2048);
            if (!buf)
               return strdup("Error: memory allocation failed");
            int pos = snprintf(buf, 2048,
                               "Similar people already exist. Which person should receive "
                               "this contact info?\n");
            for (int i = 0; i < person_count && pos < 1800; i++) {
               pos += snprintf(buf + pos, 2048 - pos, "[%d] %s (%d mentions, id:%lld)\n", i + 1,
                               persons[i].name, persons[i].mention_count, (long long)persons[i].id);
            }
            pos += snprintf(buf + pos, 2048 - pos,
                            "[%d] Create new person '%s'\n\n"
                            "Call save_contact again with entity_id parameter set to the "
                            "chosen person's id, or set entity_id to 0 to create new.",
                            person_count + 1, entity_name);
            return buf;
         }

         /* No similar people — create new entity */
         bool created = false;
         if (memory_db_entity_upsert(user_id, entity_name, "person", canonical, &created,
                                     &entity_id) != MEMORY_DB_SUCCESS)
            return strdup("Error: failed to create entity");
      }
   }

   /* entity_id == 0 means "create new" from disambiguation */
   if (entity_id == 0) {
      char canonical[64];
      memory_make_canonical_name(entity_name, canonical, sizeof(canonical));
      bool created = false;
      if (memory_db_entity_upsert(user_id, entity_name, "person", canonical, &created,
                                  &entity_id) != MEMORY_DB_SUCCESS)
         return strdup("Error: failed to create entity");
   }

   if (contacts_add(user_id, entity_id, field_type, contact_value, label) != 0)
      return strdup("Error: failed to save contact information");

   char *result = malloc(512);
   if (result)
      snprintf(result, 512, "Saved %s %s for %s%s%s%s", field_type, contact_value, entity_name,
               label[0] ? " (" : "", label, label[0] ? ")" : "");
   return result ? result : strdup("Contact saved.");
}

/* =============================================================================
 * Action: Find Contact
 * ============================================================================= */

static char *memory_action_find_contact(int user_id, const char *value) {
   if (!value || !value[0])
      return strdup("Error: find_contact requires a name to search for");

   char name[128] = "";
   tool_param_extract_base(value, name, sizeof(name));

   char field_type[32] = "";
   tool_param_extract_custom(value, "field_type", field_type, sizeof(field_type));

   contact_result_t results[10];
   int count = 0;
   contacts_find(user_id, name[0] ? name : value, field_type[0] ? field_type : NULL, results, 10,
                 &count);

   if (count <= 0)
      return strdup("No contacts found matching that name.");

   char *buf = malloc(2048);
   if (!buf)
      return strdup("Error: memory allocation failed");
   int pos = snprintf(buf, 2048, "Contact results (%d):\n", count);
   for (int i = 0; i < count && pos < 1900; i++) {
      pos += snprintf(buf + pos, 2048 - pos, "- %s: %s = %s%s%s%s [id:%lld]\n",
                      results[i].entity_name, results[i].field_type, results[i].value,
                      results[i].label[0] ? " (" : "", results[i].label,
                      results[i].label[0] ? ")" : "", (long long)results[i].contact_id);
   }
   return buf;
}

/* =============================================================================
 * Action: List Contacts
 * ============================================================================= */

static char *memory_action_list_contacts(int user_id, const char *value) {
   char field_type[32] = "";
   if (value)
      tool_param_extract_base(value, field_type, sizeof(field_type));

   contact_result_t results[20];
   int count = 0;
   contacts_list(user_id, field_type[0] ? field_type : NULL, results, 20, 0, &count);

   if (count <= 0)
      return strdup("No contacts stored.");

   char *buf = malloc(4096);
   if (!buf)
      return strdup("Error: memory allocation failed");
   int pos = snprintf(buf, 4096, "All contacts (%d):\n", count);
   for (int i = 0; i < count && pos < 3900; i++) {
      pos += snprintf(buf + pos, 4096 - pos, "- %s: %s = %s%s%s%s\n", results[i].entity_name,
                      results[i].field_type, results[i].value, results[i].label[0] ? " (" : "",
                      results[i].label, results[i].label[0] ? ")" : "");
   }
   return buf;
}

/* =============================================================================
 * Action: Merge Entities
 * ============================================================================= */

static char *memory_action_merge_entities(int user_id, const char *value) {
   if (!value || !value[0])
      return strdup("Error: merge_entities requires source_name (query) and target_name");

   char source_name[128] = "";
   tool_param_extract_base(value, source_name, sizeof(source_name));

   char target_name[128] = "";
   tool_param_extract_custom(value, "target_name", target_name, sizeof(target_name));

   if (!source_name[0] || !target_name[0])
      return strdup("Error: merge_entities requires both source_name (query) and target_name");

   /* Run prompt-injection filter on both names before any DB lookup —
    * mirrors what the resolver path will do at extraction time, and
    * prevents a poisoned name from sliding into a downstream
    * canonical_name field via a misdirected merge. */
   if (memory_filter_check(source_name) || memory_filter_check(target_name)) {
      return strdup("Error: entity name failed prompt-injection filter");
   }

   /* Look up both entities by canonical name */
   char src_canonical[64], tgt_canonical[64];
   memory_make_canonical_name(source_name, src_canonical, sizeof(src_canonical));
   memory_make_canonical_name(target_name, tgt_canonical, sizeof(tgt_canonical));

   memory_entity_t src_entity, tgt_entity;
   if (memory_db_entity_get_by_name(user_id, src_canonical, &src_entity) != MEMORY_DB_SUCCESS)
      return strdup("Error: source entity not found");
   if (memory_db_entity_get_by_name(user_id, tgt_canonical, &tgt_entity) != MEMORY_DB_SUCCESS)
      return strdup("Error: target entity not found");

   /* Soft-link by default (entity-merge Phase 1, design §14): set
    * canonical_id on the source row + audit-row insert.  The link is
    * reversible — operators promote to permanent merge via
    * `dawn-admin memory entity consolidate` (Phase 3). */
   int64_t link_id = 0;
   int rc = memory_db_entity_alias_link(user_id, src_entity.id, tgt_entity.id, "soft",
                                        "llm-tool-action",
                                        /* composite_score */ -1.0f,
                                        /* evidence_json */ NULL, &link_id);
   if (rc == MEMORY_DB_SUCCESS) {
      char *result = malloc(768);
      if (result)
         snprintf(result, 768,
                  "Linked '%s' as a soft alias of '%s' (link id %lld). "
                  "The '%s' entity is preserved and will be resolved to '%s' on lookup. "
                  "Use 'split' to undo, or 'dawn-admin memory entity consolidate' to make it "
                  "permanent.",
                  source_name, target_name, (long long)link_id, source_name, target_name);
      return result ? result : strdup("Entities soft-linked.");
   } else if (rc == MEMORY_DB_NOT_FOUND) {
      /* Race: entity deleted between lookup and link */
      OLOG_WARNING("memory_callback: merge_entities race — entity vanished between lookup and "
                   "link (source='%s', target='%s')",
                   source_name, target_name);
      return strdup("Error: one or both entities no longer exist (may have been deleted)");
   } else {
      /* alias_link refuses self-link, source-with-existing-aliases (would
       * orphan the equivalence class), and DB failures.  Distinct return
       * codes aren't surfaced; report the operation refused. */
      return strdup("Error: cannot link these entities "
                    "(source may already be a canonical with its own aliases, or "
                    "source and target are the same)");
   }
}

/* =============================================================================
 * Action: Delete Contact
 * ============================================================================= */

static char *memory_action_delete_contact(int user_id, const char *value) {
   if (!value || !value[0])
      return strdup("Error: delete_contact requires a contact ID");

   char id_str[32] = "";
   tool_param_extract_base(value, id_str, sizeof(id_str));
   int64_t contact_id = strtoll(id_str[0] ? id_str : value, NULL, 10);
   if (contact_id <= 0)
      return strdup("Error: invalid contact ID");

   if (contacts_delete(user_id, contact_id) != 0)
      return strdup("Error: contact not found or already deleted");

   return strdup("Contact deleted.");
}

/* =============================================================================
 * Main Callback
 * ============================================================================= */

/* Privacy gate for an explicit `remember` (v75).
 *
 * `is_private` means "no AUTOMATIC extraction from this conversation".  It has
 * never covered the memory tool, because memory_action_remember() takes only
 * (user_id, value) and has no conversation to test — so a `remember` issued
 * inside a private conversation wrote through, and that is how private research
 * reached long-term memory.
 *
 * Deliberately a CONFIRMATION, not a block.  "Remember my daughter's birthday is
 * the 14th" said inside a private conversation is a legitimate instruction, and
 * silently discarding it is the worse failure mode: the user believes it was
 * saved and finds out months later.  What must not happen is the MODEL deciding
 * unprompted to persist private material.  Nothing here can distinguish those
 * two, so it hands the decision to the person who can.
 *
 * @return NULL to proceed, or an owned refusal string to return to the caller.
 */
static char *memory_private_confirm_gate(int user_id, const char *value) {
#ifdef ENABLE_AUTH
   session_t *session = session_get_command_context();
   if (session == NULL) {
      return NULL; /* no conversation context (scheduler, MQTT) — nothing to gate */
   }
   int64_t conv_id = atomic_load(&session->stream_conversation_id);
   if (conv_id <= 0) {
      return NULL;
   }
   bool is_private = false;
   if (conv_db_is_private(conv_id, user_id, &is_private) != AUTH_DB_SUCCESS || !is_private) {
      /* On a read error, proceed: a private conversation is already exempt from
       * extraction, so the residual exposure here is one explicit fact, whereas
       * failing closed would silently swallow every deliberate save. */
      return NULL;
   }

   char confirm[8] = "";
   if (tool_param_extract_custom((char *)value, "confirm_private", confirm, sizeof(confirm)) &&
       strcmp(confirm, "true") == 0) {
      OLOG_INFO("memory: confirmed save from private conversation %lld", (long long)conv_id);
      return NULL;
   }

   OLOG_INFO("memory: refused unconfirmed save from private conversation %lld", (long long)conv_id);
   return strdup(
       "Not saved — this conversation is private, so nothing leaves it unless the user wants it "
       "to. If the user ASKED you to remember this, they have already decided: call 'remember' "
       "again with confirm_private=true and don't ask them to repeat themselves. Only if saving "
       "was your own idea, ask them first.");
#else
   (void)user_id;
   (void)value;
   return NULL;
#endif
}

char *memoryCallback(const char *actionName, char *value, int *should_respond) {
   if (should_respond) {
      *should_respond = 1;
   }

   /* Check if memory system is enabled */
   if (!g_config.memory.enabled) {
      return strdup("Memory system is disabled.");
   }

   /* Get current user ID */
   int user_id = get_current_user_id();
   if (user_id <= 0) {
      return strdup("Memory system requires authentication. Please log in.");
   }

   if (!actionName) {
      return strdup("Invalid memory action.");
   }

   OLOG_INFO("memory_callback: action='%s', value='%s', user_id=%d", actionName,
             value ? value : "(null)", user_id);

   if (strcmp(actionName, "search") == 0) {
      /* Extract base keywords and optional time_range / category / as_of /
       * include_historical / with_source from encoded value. */
      char keywords[512] = "";
      time_t since_ts = 0;
      char category[MEMORY_CATEGORY_MAX] = "";
      int64_t as_of_ts = 0;
      bool include_historical = false;
      bool with_source = false;

      if (value) {
         tool_param_extract_base(value, keywords, sizeof(keywords));

         char time_range[32] = "";
         if (tool_param_extract_custom(value, "time_range", time_range, sizeof(time_range))) {
            time_t seconds = parse_time_period(time_range);
            if (seconds > 0) {
               since_ts = time(NULL) - seconds;
               if (since_ts < 0)
                  since_ts = 0;
            }
         }

         /* category param: validate against canonical taxonomy; ignore unknowns. */
         char raw_cat[MEMORY_CATEGORY_MAX] = "";
         if (tool_param_extract_custom(value, "category", raw_cat, sizeof(raw_cat)) && raw_cat[0]) {
            for (int i = 0; i < MEMORY_FACT_CATEGORY_COUNT; i++) {
               if (strcmp(raw_cat, MEMORY_FACT_CATEGORIES[i]) == 0) {
                  strncpy(category, raw_cat, sizeof(category) - 1);
                  category[sizeof(category) - 1] = '\0';
                  break;
               }
            }
            if (!category[0]) {
               OLOG_INFO("memory_callback: ignoring unknown category '%s'", raw_cat);
            }
         }

         /* as_of: ISO-8601 date for relation validity at a historical point. */
         char as_of_str[32] = "";
         if (tool_param_extract_custom(value, "as_of", as_of_str, sizeof(as_of_str)) &&
             as_of_str[0]) {
            struct tm tm = { 0 };
            char *end = strptime(as_of_str, "%Y-%m-%d", &tm);
            if (!end) {
               memset(&tm, 0, sizeof(tm));
               end = strptime(as_of_str, "%Y", &tm);
               if (end) {
                  tm.tm_mday = 1;
                  tm.tm_mon = 0;
               }
            }
            if (end) {
               time_t t = timegm(&tm);
               if (t != (time_t)-1)
                  as_of_ts = (int64_t)t;
            }
         }

         /* include_historical: "true" / "false" string. */
         char hist[8] = "";
         if (tool_param_extract_custom(value, "include_historical", hist, sizeof(hist))) {
            include_historical = (strcmp(hist, "true") == 0);
         }

         /* with_source: return verbatim excerpts for each fact. */
         char wsrc[8] = "";
         if (tool_param_extract_custom(value, "with_source", wsrc, sizeof(wsrc))) {
            with_source = (strcmp(wsrc, "true") == 0);
         }
      }

      /* source_budget from dawn.toml [memory] source_budget_chars; 0 falls
       * back to MEMORY_SOURCE_BUDGET_CHARS inside the action.  Bench harness
       * invokes memory_action_search() directly with its own override. */
      return memory_action_search(user_id, keywords[0] ? keywords : NULL, since_ts,
                                  category[0] ? category : NULL, as_of_ts, include_historical,
                                  with_source, g_config.memory.source_budget_chars);
   } else if (strcmp(actionName, "remember") == 0) {
      char *gate = memory_private_confirm_gate(user_id, value);
      if (gate != NULL) {
         return gate; /* private conversation, not yet confirmed */
      }
      /* Strip the packed custom-param suffix before the text is stored.
       * TOOL_MAPS_TO_CUSTOM params ride INSIDE `value` as
       * "base::field::val" (tool_registry.h), and remember stores its input
       * verbatim as fact_text.  `remember` had no custom param until
       * confirm_private, so passing `value` straight through used to be safe;
       * now the confirmed retry — the very thing the refusal string tells the
       * model to do — would persist "…the 14th::confirm_private::true" as the
       * fact, poisoning its embedding, its dedup hash and every later recall.
       *
       * Trimmed at OUR marker specifically rather than via
       * tool_param_extract_base(), which cuts at the FIRST "::" — fine for the
       * ID lists `forget` passes, but a fact is free-form user text and may
       * legitimately contain "::" ("the ratio is 3::1"), which base-extraction
       * would silently truncate. */
      static const char kConfirmMarker[] = "::confirm_private::";
      char *marker = strstr(value, kConfirmMarker);
      char *trimmed = NULL;
      if (marker != NULL) {
         size_t base_len = (size_t)(marker - value);
         trimmed = strndup(value, base_len);
      }
      char *res = memory_action_remember(user_id, trimmed ? trimmed : value);
      free(trimmed);
      return res;
   } else if (strcmp(actionName, "forget") == 0) {
      /* IDs are the base value; optional replaced_by switches delete -> supersede (merge).
       * Base-extract so the ID parser doesn't choke on the ::replaced_by:: suffix. */
      char forget_ids[1024] = "";
      char rb[32] = "";
      tool_param_extract_base(value, forget_ids, sizeof(forget_ids));
      int64_t replaced_by = 0;
      if (tool_param_extract_custom(value, "replaced_by", rb, sizeof(rb)) && rb[0]) {
         replaced_by = (int64_t)strtoll(rb, NULL, 10);
         if (replaced_by < 0)
            replaced_by = 0;
      }
      return memory_action_forget(user_id, forget_ids, replaced_by);
   } else if (strcmp(actionName, "get") == 0) {
      /* Deterministic exact-fetch by numeric ID(s); base-extract so the parser
       * ignores any encoded suffix. */
      char get_ids[1024] = "";
      tool_param_extract_base(value, get_ids, sizeof(get_ids));
      return memory_action_get(user_id, get_ids);
   } else if (strcmp(actionName, "find_duplicates") == 0) {
      return memory_action_find_duplicates(user_id, value);
   } else if (strcmp(actionName, "recent") == 0) {
      /* Bundle 3 (2026-05-13): extract the time-period window from the base
       * value, plus the new 'before' / 'sort' / 'limit' modifiers from
       * custom fields.  Pre-Bundle-3 the whole value string was passed
       * directly to memory_action_recent which then parsed it as a time
       * period — that's still the behavior when no modifiers are present. */
      char period[32] = "";
      tool_param_extract_base(value, period, sizeof(period));

      char before[32] = "";
      if (value) {
         tool_param_extract_custom(value, "before", before, sizeof(before));
      }

      bool sort_asc = false;
      char sort_str[16] = "";
      if (value && tool_param_extract_custom(value, "sort", sort_str, sizeof(sort_str))) {
         sort_asc = (strcmp(sort_str, "oldest") == 0);
      }

      int limit = 0;
      char limit_str[16] = "";
      if (value && tool_param_extract_custom(value, "limit", limit_str, sizeof(limit_str)) &&
          limit_str[0]) {
         /* strtol over atoi: atoi has implementation-defined behavior on
          * overflow (GCC saturates to INT_MAX but the standard doesn't
          * require it).  strtol with ERANGE check + explicit range bounds
          * matches the bounded-numeric pattern in parse_time_period. */
         char *endptr = NULL;
         errno = 0;
         long parsed = strtol(limit_str, &endptr, 10);
         if (errno == 0 && endptr != limit_str && parsed > 0 && parsed <= MEMORY_RECENT_LIMIT_MAX) {
            limit = (int)parsed;
         }
         /* Out-of-range / malformed → limit stays 0 → action layer uses
          * defaults.  memory_action_recent clamps in-range values
          * internally as a second gate. */
      }

      bool with_source = false;
      if (value) {
         char wsrc[8] = "";
         if (tool_param_extract_custom(value, "with_source", wsrc, sizeof(wsrc))) {
            with_source = (strcmp(wsrc, "true") == 0);
         }
      }
      return memory_action_recent(user_id, period[0] ? period : NULL, before[0] ? before : NULL,
                                  sort_asc, limit, with_source,
                                  g_config.memory.source_budget_chars);
   } else if (strcmp(actionName, "save_contact") == 0) {
      return memory_action_save_contact(user_id, value);
   } else if (strcmp(actionName, "find_contact") == 0) {
      return memory_action_find_contact(user_id, value);
   } else if (strcmp(actionName, "list_contacts") == 0) {
      return memory_action_list_contacts(user_id, value);
   } else if (strcmp(actionName, "merge_entities") == 0) {
      return memory_action_merge_entities(user_id, value);
   } else if (strcmp(actionName, "delete_contact") == 0) {
      return memory_action_delete_contact(user_id, value);
   } else {
      char *msg = malloc(128);
      if (msg) {
         snprintf(msg, 128, "Unknown memory action: '%s'", actionName);
         return msg;
      }
      return strdup("Unknown memory action.");
   }
}
