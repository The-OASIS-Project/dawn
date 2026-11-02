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

#define MEMORY_EXTRACTION_INTERNAL_ALLOWED
#include "memory/memory_extraction.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/automated_event.h"
#include "core/buf_printf.h"
#include "core/memory_filter.h"
#include "core/session_manager.h"
#include "core/strbuf.h"
#include "core/user_tz.h"
#include "dawn_error.h"
#include "llm/llm_cache_monitor.h"
#include "llm/llm_interface.h"
#include "llm/llm_tools.h"
#include "llm/llm_turn_blocks.h"
#include "logging.h"
#include "memory/memory_candidates.h"
#include "memory/memory_db.h"
#include "memory/memory_db_provenance.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_embeddings_internal.h"
#include "memory/memory_extraction_input.h"
#include "memory/memory_extraction_internal.h"
#include "memory/memory_forgotten.h"
#include "memory/memory_history_loader.h"
#include "memory/memory_note_guard.h"
#include "memory/memory_types.h"
#include "prompts.h"
#include "utils/string_utils.h"

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
   int64_t first_msg_id;                  /* the batch's first saved row (0: none) */
   int outside;                           /* outside_kind_t its history carries */
   int64_t checked_max_id;                /* highest row the history held (0: none) */
   bool has_fallback;                     /* Whether fallback LLM info is available */
   memory_extraction_fallback_t fallback; /* Session's active LLM for retry */
} extraction_context_t;

/* Set only by the benchmark harness (bench_retrieval --dump-candidates). */
static memory_extraction_reply_hook_t s_reply_hook;

void memory_extraction_set_reply_hook(memory_extraction_reply_hook_t hook) {
   s_reply_hook = hook;
}

/* =============================================================================
 * Helper: Build existing profile string
 * ============================================================================= */

#define EXISTING_PROFILE_BUF 4096

static char *build_existing_profile(int user_id) {
   char *profile = malloc(EXISTING_PROFILE_BUF);
   if (!profile)
      return strdup("(none)");

   size_t off = 0;
   size_t rem = EXISTING_PROFILE_BUF;

   /* What the user already told DAWN.  Facts and preferences outside text
    * taught stay out: this batch would read them without taking their kind. */
   memory_preference_t prefs[10];
   int pref_count = 0;
   memory_db_pref_list_untainted(user_id, prefs, 10, 0, &pref_count);

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

   bool facts_head = false;
   for (int i = 0; i < fact_count && rem > 1; i++) {
      if (outside_is_tainted(facts[i].outside)) {
         continue;
      }
      if (!facts_head) {
         BUF_PRINTF(profile, off, rem, "%sKnown facts:\n", off > 0 ? "\n" : "");
         facts_head = true;
      }
      BUF_PRINTF(profile, off, rem, "- %s\n", facts[i].fact_text);
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
      snprintf(profile, EXISTING_PROFILE_BUF, "(none)");
   }

   return profile;
}

/* =============================================================================
 * WebUI broadcast (provided by webui_server.c when ENABLE_WEBUI)
 * ============================================================================= */

#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif

/* =============================================================================
 * Helper: Parse extraction response
 * ============================================================================= */

static void process_extraction_response(int user_id,
                                        int64_t conversation_id,
                                        const char *session_id,
                                        const char *response_text,
                                        int message_count,
                                        int duration_seconds,
                                        const memory_provenance_t *prov,
                                        const memory_forgotten_list_t *forgotten) {
   if (!response_text) {
      OLOG_WARNING("memory_extraction: NULL response from LLM");
      return;
   }

   /* Extract JSON from response (handles markdown blocks, preamble text, etc.) */
   struct json_object *root = memory_extraction_parse_json(response_text);
   if (!root) {
      OLOG_WARNING("memory_extraction: Failed to parse LLM response as JSON");
      OLOG_WARNING("memory_extraction: Response preview: %.200s...", response_text);
      return;
   }

   memory_candidates_t cands = { 0 };
   memory_store_t *st = memory_store_open(user_id, conversation_id, prov, false);
   if (!st || memory_candidates_parse(root, user_id, &cands) != SUCCESS) {
      OLOG_WARNING("memory_extraction: out of memory reading the extraction reply");
      memory_store_close(st);
      memory_candidates_free(&cands);
      json_object_put(root);
      return;
   }
   const int64_t conv_created_at = memory_store_created_at(st);

   /* Entities first: each fact names its subject, resolved against them. */
   memory_store_entities(st, &cands);

   for (int i = 0; i < cands.count; i++) {
      const char *text = cands.items[i].text;
      /* No substring filter on extracted facts — LLM paraphrase, not raw user
       * input.  See INJECTION_FILTER.md §"Filtering at the trust boundary,
       * not after paraphrase". */

      /* A fact the user forgot, said again: skipped with its relations.  One
       * that only mentions it is stored without that part (and without
       * relations: they may be the forgotten part). */
      if (memory_forgotten_is_repeat(forgotten, text, conversation_id)) {
         continue;
      }
      const char *reworded = memory_forgotten_rewritten(forgotten, text);
      const memory_store_opts_t opts = { .text = reworded, .no_relations = reworded != NULL };
      memory_store_candidate(st, &cands, i, &opts, NULL);
   }

   /* Process preferences.  One from a batch that read outside text is stored
    * with that kind, never over a value that isn't (memory_db_pref_upsert),
    * and stays out of the session-start block. */
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
            /* A forgotten fact said again: skipped. */
            if (!new_fact || memory_forgotten_is_repeat(forgotten, new_fact, conversation_id))
               continue;
            const char *reworded = memory_forgotten_rewritten(forgotten, new_fact);
            if (reworded)
               new_fact = reworded;
            /* A correction of a forgotten fact: skipped.  That one was deleted, so
             * another row would be marked replaced in its place; the new value
             * comes as a fact of its own. */
            if (memory_forgotten_names(forgotten, old_fact))
               continue;

            /* Find and supersede old fact */
            memory_fact_t similar[3];
            int similar_count = 0;
            memory_db_fact_find_similar(user_id, old_fact, similar, 3, &similar_count);

            if (similar_count > 0) {
               /* Create new fact and supersede old one */
               int64_t new_id = 0;
               memory_db_fact_create_at(user_id, new_fact, 0.9f, "explicit", prov, conv_created_at,
                                        &new_id);
               /* From a batch that read outside text, a correction of a fact
                * that isn't from such a batch is stored beside it, never
                * replacing it. */
               const bool beside = prov && outside_is_tainted(prov->outside) &&
                                   !outside_is_tainted(similar[0].outside);
               if (new_id > 0) {
                  /* The old fact is an older value the correction replaced
                   * (back if the correction is forgotten); what it stated stays
                   * open, since a correction rarely retracts all of it.
                   * Refused (logged) when the old one was already superseded. */
                  if (beside) {
                     OLOG_INFO("memory_extraction: correction of fact %lld kept beside it "
                               "(outside text read)",
                               (long long)similar[0].id);
                  } else if (memory_db_fact_correct(similar[0].id, new_id, user_id) !=
                             MEMORY_DB_SUCCESS) {
                     OLOG_WARNING("memory_extraction: correction of fact %lld not applied "
                                  "(already merged or replaced)",
                                  (long long)similar[0].id);
                  }
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

   /* Relations live inside each fact's "relations[]".  A top-level array is
    * ignored, with a warning, so a model back on the old shape is noticed. */
   struct json_object *legacy_relations_arr;
   if (json_object_object_get_ex(root, "relations", &legacy_relations_arr) &&
       json_object_is_type(legacy_relations_arr, json_type_array) &&
       json_object_array_length(legacy_relations_arr) > 0) {
      OLOG_WARNING("memory_extraction: ignoring %d top-level relations[] entries — "
                   "new prompt schema (v47) puts relations inside each fact's relations[] array. "
                   "The LLM may not be honoring the schema; review extraction output.",
                   json_object_array_length(legacy_relations_arr));
   }

   /* The merge check on the entities this batch created, and the caches its
    * writes made stale. */
   memory_store_close(st);
   memory_candidates_free(&cands);

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
         safe_strscpy(safe_title, title);
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
   safe_strscpy(fb->endpoint, cfg.endpoint);
   safe_strscpy(fb->model, cfg.model);
}

/* =============================================================================
 * Extraction Thread
 * ============================================================================= */

void memory_extraction_anchor_line(int user_id, int64_t conversation_id, char *out, size_t out_sz) {
   out[0] = '\0';
   int64_t anchor_ts = ANCHOR_DATE_NONE;
   if (conversation_id <= 0 ||
       conv_db_get_anchor_date(conversation_id, &anchor_ts) != AUTH_DB_SUCCESS ||
       anchor_ts == ANCHOR_DATE_NONE)
      return;
   /* The user's date: "yesterday" is theirs, not UTC's. */
   struct tm tm_local;
   tz_local(user_tz(user_id), (time_t)anchor_ts, &tm_local);
   char date_buf[16]; /* "YYYY-MM-DD\0" needs 11; 16 is conventional. */
   if (strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm_local) != 0)
      snprintf(out, out_sz, "Conversation anchor: %s\n\n", date_buf);
}

static void *extraction_thread(void *arg) {
   extraction_context_t *ctx = (extraction_context_t *)arg;

   OLOG_INFO("memory_extraction: starting for user %d, session %s", ctx->user_id, ctx->session_id);

   /* Build existing profile */
   char *existing_profile = build_existing_profile(ctx->user_id);

   /* What the user forgot after this batch began: what the model returns is
    * checked against it.  Unreadable: not extracted now (the cursor stays, so
    * recovery tries again), rather than learn it back. */
   memory_forgotten_list_t *forgotten = calloc(1, sizeof(*forgotten));
   /* A history never saved: it began duration_seconds ago. */
   const int64_t since = (int64_t)time(NULL) - ctx->duration_seconds;
   if (!forgotten || memory_forgotten_load(ctx->user_id, ctx->conversation_id, ctx->first_msg_id,
                                           since, forgotten) != MEMORY_DB_SUCCESS) {
      OLOG_WARNING("memory_extraction: conv %lld: forgotten facts unreadable; not extracting now",
                   (long long)ctx->conversation_id);
      goto cleanup;
   }

   char anchor_line[MEMORY_EXTRACTION_ANCHOR_LINE_LEN];
   memory_extraction_anchor_line(ctx->user_id, ctx->conversation_id, anchor_line,
                                 sizeof(anchor_line));

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
   /* Reasoning for this call ([memory] extraction_effort).  The session-model
    * fallback below and the other memory-model calls keep the cheapest setting. */
   if (strcmp(g_config.memory.extraction_effort, "off") != 0) {
      safe_strscpy(extraction_config.utility_effort, g_config.memory.extraction_effort);
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
   const char *expiry_block = g_config.memory.expire_enabled ? MEMORY_EXTRACTION_EXPIRY_BLOCK : "";

   char *prompt = memory_extraction_build_prompt(anchor_line, ctx->conversation_json,
                                                 existing_profile, expiry_block);
   if (!prompt) {
      OLOG_ERROR("memory_extraction: failed to allocate prompt");
      goto cleanup;
   }

   /* Call LLM for extraction using configured provider/model */
   char *response = NULL;

   /* Build a minimal conversation history with just the extraction prompt */
   struct json_object *extraction_history = json_object_new_array();
   struct json_object *user_msg = json_object_new_object();
   json_object_object_add(user_msg, "role", json_object_new_string("user"));
   json_object_object_add(user_msg, "content", json_object_new_string(prompt));
   json_object_array_add(extraction_history, user_msg);

   /* Use the configured LLM for extraction.  Extraction runs over untrusted
    * conversation content, so bracket the call with the config-independent
    * tools-off guard (belt-and-suspenders alongside suppress_tools). */
   llm_tools_suppress_push();
   const int kind_prev = llm_cache_monitor_push_kind(LLM_CALL_EXTRACTION);
   response = llm_chat_completion_with_config(extraction_history, prompt, &extraction_config);
   llm_tools_suppress_pop();

   /* Capture primary's transient status BEFORE any fallback runs.  The
    * fallback call resets llm_last_error() at entry, so if the primary
    * failed transient (cloud unreachable) and the fallback then fails
    * for a non-transient reason (local model returned empty), the final
    * llm_last_error() reading would lose the primary's transient signal
    * and recovery would incorrectly take the give-up branch. */
   bool primary_was_transient = (response == NULL && llm_last_error() == LLM_ERR_TRANSIENT_NETWORK);

   /* If primary model failed and we have a fallback, retry with the session's active model */
   bool used_fallback = false;
   llm_resolved_config_t fallback_config = { 0 }; /* the model that answered, when it did */
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

         fallback_config.type = ctx->fallback.type;
         fallback_config.cloud_provider = ctx->fallback.cloud_provider;
         fallback_config.endpoint = ctx->fallback.endpoint;
         fallback_config.model = ctx->fallback.model;
         fallback_config.suppress_tools = true;
         safe_strscpy(fallback_config.thinking_mode, "disabled");

         fallback_config.timeout_ms = g_config.memory.extraction_timeout_ms;

         /* Resolve API key for cloud providers */
         if (fallback_config.type == LLM_CLOUD) {
            if (fallback_config.cloud_provider == CLOUD_PROVIDER_OPENAI)
               fallback_config.api_key = g_secrets.openai_api_key;
            else if (fallback_config.cloud_provider == CLOUD_PROVIDER_CLAUDE)
               fallback_config.api_key = g_secrets.claude_api_key;
            else if (fallback_config.cloud_provider == CLOUD_PROVIDER_GEMINI)
               fallback_config.api_key = g_secrets.gemini_api_key;
            else if (fallback_config.cloud_provider == CLOUD_PROVIDER_OPENROUTER) {
               /* Session provider is OpenRouter: its model is already a "vendor/model"
                * slug, so keep it (fall back to the main OpenRouter default if unset). */
               fallback_config.api_key = g_secrets.openrouter_api_key;
               fallback_config.endpoint = OPENROUTER_URL;
               if (!fallback_config.model || !fallback_config.model[0])
                  fallback_config.model = llm_get_default_openrouter_model();
            }
         }

         llm_tools_suppress_push();
         response = llm_chat_completion_with_config(extraction_history, prompt, &fallback_config);
         llm_tools_suppress_pop();
         if (response) {
            used_fallback = true;
         }
      }
   }

   llm_cache_monitor_pop_kind(kind_prev); /* primary and fallback both tagged */
   json_object_put(extraction_history);

#ifdef ENABLE_WEBUI
   /* Recovery-triggered extractions process old, idle conversations the user
    * has long since moved on from.  Surfacing a noisy "extraction failed"
    * toast for those is just clutter — log only, don't notify. */
   bool is_recovery_run = (strncmp(ctx->session_id, "recovery_", 9) == 0);
#endif

   /* Source range for provenance: (last_extracted_msg_id + 1, the highest row
    * the batch's history held), the cursor's last row read after the LLM
    * returns (a concurrent extraction may have moved it). */
   memory_provenance_t prov = { .outside = ctx->outside }; /* no source range yet */
   if (ctx->conversation_id > 0 && response) {
      int64_t last_id = 0;
      memory_db_get_last_extracted_msg_id(ctx->conversation_id, &last_id);
      int64_t max_id = 0;
      conv_db_get_max_msg_id(ctx->conversation_id, ctx->user_id, &max_id);
      /* No further than the history the batch read: a row saved during the
       * call is read next time. */
      if (ctx->checked_max_id > 0 && max_id > ctx->checked_max_id) {
         max_id = ctx->checked_max_id;
      }
      if (max_id > 0 && max_id >= last_id) {
         prov.conv_id = ctx->conversation_id;
         prov.msg_id_start = last_id + 1;
         prov.msg_id_end = max_id;
      }
   }

   /* The LLM call above can take tens of seconds; the user may have marked the
    * conversation private (or deleted it) meanwhile.  Re-check just before
    * anything is written, fail closed, and leave the cursor alone.  A toggle
    * during the write below (which embeds each new row, so it can take seconds
    * with a remote embedding provider) lets that run finish: the rows it writes
    * count as learned, and forgetting them waits for it to end
    * (memory_extraction_hold_user). */
   bool revoked = false;
   /* What it returned that states a forgotten fact again isn't stored (out of
    * memory: nothing is now; the cursor stays, and recovery tries again).
    * Before the privacy re-check below: the check is another model call. */
   if (response &&
       memory_forgotten_screen(forgotten, response,
                               used_fallback ? &fallback_config : &extraction_config) != SUCCESS) {
      OLOG_WARNING("memory_extraction: conv %lld: not stored now (forgotten-fact check failed)",
                   (long long)ctx->conversation_id);
      free(response);
      response = NULL;
      revoked = true;
   }
#ifdef ENABLE_AUTH
   if (!revoked && response && ctx->conversation_id > 0) {
      bool now_private = false;
      int priv_rc = conv_db_is_private(ctx->conversation_id, ctx->user_id, &now_private);
      if (priv_rc != AUTH_DB_SUCCESS || now_private) {
         OLOG_INFO("memory_extraction: discarding result - conversation %lld became %s during "
                   "extraction",
                   (long long)ctx->conversation_id,
                   priv_rc == AUTH_DB_SUCCESS ? "private" : "unreadable");
         free(response);
         response = NULL;
         revoked = true;
      }
   }
#endif

   if (revoked) {
      /* nothing written; see above */
   } else if (response) {
      if (s_reply_hook) {
         s_reply_hook(ctx->conversation_id, response);
      }
      process_extraction_response(ctx->user_id, ctx->conversation_id, ctx->session_id, response,
                                  ctx->new_message_count, ctx->duration_seconds, &prov, forgotten);
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

      /* Update extraction high-water mark on success, to the end of the range
       * above: rows saved during the call stay to be read. */
      if (ctx->conversation_id > 0) {
         /* What this batch read stays with the conversation (its floor): a
          * later batch reads only its own rows. */
         if (memory_db_set_last_extracted(ctx->conversation_id, ctx->message_count, prov.msg_id_end,
                                          ctx->outside) != MEMORY_DB_SUCCESS) {
            OLOG_WARNING("memory_extraction: conv %lld: cursor not advanced",
                         (long long)ctx->conversation_id);
         }
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
      extraction_record_outcome(ctx->user_id, was_transient);
   }

   free(prompt);

cleanup:
   memory_forgotten_free(forgotten);
   free(forgotten);
   free(existing_profile);
   free(ctx->conversation_json);

   /* Clear in-progress flag - save user_id before freeing ctx */
   int saved_user_id = ctx->user_id;
   free(ctx);

   extraction_slot_release(saved_user_id);

   OLOG_INFO("memory_extraction: completed for user %d", saved_user_id);
   return NULL;
}

/* =============================================================================
 * Public API
 * ============================================================================= */


/* A request refused before it started: with @p have_slot (a queued run's slot,
 * claimed when it was taken) the slot goes back. */
static int trigger_skip(int user_id, bool have_slot, int rc) {
   if (have_slot) {
      extraction_slot_release(user_id);
   }
   return rc;
}

/* memory_trigger_extraction(), or a queued run (@p have_slot: its slot is
 * already claimed, and every return lets it go or hands it to the thread). */
static int trigger_extraction(int user_id,
                              int64_t conversation_id,
                              const char *session_id_str,
                              struct json_object *conversation_history,
                              int message_count,
                              int duration_seconds,
                              const memory_extraction_fallback_t *fallback,
                              bool have_slot) {
   if (user_id <= 0 || !conversation_history) {
      return trigger_skip(user_id, have_slot, 1);
   }

   if (!g_config.memory.enabled) {
      return trigger_skip(user_id, have_slot, 1);
   }

#ifdef ENABLE_AUTH
   /* Every caller that knows the conversation passes it, so this is the one
    * place the "never extract" rules are enforced for all of them.  Re-read from
    * the DB (a cached flag can lag set_private).  Fail closed: if the row can't be
    * read (deleted, not this user's, DB error) nothing is extracted; memory
    * recovery re-extracts eligible conversations from the DB later. */
   if (conversation_id > 0) {
      bool is_private = false;
      int priv_rc = conv_db_is_private(conversation_id, user_id, &is_private);
      if (priv_rc != AUTH_DB_SUCCESS || is_private) {
         OLOG_INFO("memory_extraction: skipping - conversation %lld is %s",
                   (long long)conversation_id,
                   priv_rc == AUTH_DB_SUCCESS ? "private" : "unreadable");
         return trigger_skip(user_id, have_slot, 0);
      }
      /* A background job's transcript is research the model did, not what the
       * user said; memory comes from the parent conversation it reports into. */
      job_record_t job_rec;
      int job_rc = conv_db_job_get(conversation_id, user_id, &job_rec);
      if (job_rc != AUTH_DB_NOT_FOUND) {
         OLOG_INFO("memory_extraction: skipping - conversation %lld is %s",
                   (long long)conversation_id,
                   job_rc == AUTH_DB_SUCCESS ? "a background job" : "unreadable (job check)");
         return trigger_skip(user_id, have_slot, 0);
      }
   }
#endif

   /* Skip if too few messages.  Mark the conversation as up-to-date so
    * recovery scans don't keep re-evaluating a structurally inextractable
    * conversation forever.  If the user later adds a 2nd message the
    * counter advances back to zero on the new live extraction. */
   if (message_count < 2) {
      OLOG_INFO("memory_extraction: skipping - too few messages (%d)", message_count);
      if (conversation_id > 0) {
         memory_db_set_last_extracted(conversation_id, message_count, 0,
                                      OUTSIDE_NONE); /* 0 = leave msg_id cursor unchanged */
      }
      return trigger_skip(user_id, have_slot, 0);
   }

   /* ID-based incremental extraction cursor.
    * Also maintain the count-based cursor for one release cycle (back-compat
    * verification — lets spot-checking confirm both cursors agree). */
   int last_extracted = 0;
   int64_t last_msg_id = 0;
   /* What the conversation read before this batch (its floor), and DAWN's
    * digest of outside content when a briefing is in it. */
   int floor = OUTSIDE_NONE;
   if (conversation_id > 0) {
      /* A scheduled briefing is DAWN's digest of outside content (news,
       * weather, email, calendar), not something the user said: it counts as
       * extracted, so only the user's replies are read and a briefing nobody
       * answered never reaches the model (the no-new-messages skip below). */
      int64_t briefing_end = 0;
      bool briefing_replied = false;
      if (conv_db_briefing_mark_extracted(conversation_id, user_id, &briefing_end,
                                          &briefing_replied) != AUTH_DB_SUCCESS) {
         /* Fail closed: unchecked, a briefing would reach the model; memory
          * recovery tries the conversation again later. */
         OLOG_WARNING("memory_extraction: skipping conv %lld: briefing check failed",
                      (long long)conversation_id);
         return trigger_skip(user_id, have_slot, 0);
      }
      if (conv_db_get_outside(conversation_id, user_id, &floor) != AUTH_DB_SUCCESS) {
         floor = OUTSIDE_OTHER; /* unreadable: fail closed, as the checks beside it */
      }
      if (briefing_end > 0) {
         floor = outside_combine(floor, OUTSIDE_OTHER);
         OLOG_INFO("memory_extraction: conv %lld: briefing rows through %lld not extracted%s",
                   (long long)conversation_id, (long long)briefing_end,
                   briefing_replied ? "" : " (no reply)");
      }
      memory_db_get_last_extracted(conversation_id, &last_extracted);
      memory_db_get_last_extracted_msg_id(conversation_id, &last_msg_id);

      /* ID-based early-skip: compare max DB row ID against cursor. */
      int64_t max_msg_id = 0;
      conv_db_get_max_msg_id(conversation_id, user_id, &max_msg_id);
      if (max_msg_id > 0 && max_msg_id <= last_msg_id) {
         OLOG_INFO("memory_extraction: skipping — no new messages (max_id=%lld, last=%lld)",
                   (long long)max_msg_id, (long long)last_msg_id);
         return trigger_skip(user_id, have_slot, 0);
      }
   }

   int new_messages = message_count - last_extracted;

   /* Check concurrency limits and acquire extraction slot.  Refused (the
    * user's extraction is running or held, or every slot is busy): a
    * conversation waits in the queue for the next release; recovery's own
    * requests retry on its pass instead. */
   if (!have_slot &&
       !extraction_slot_claim_or_queue(user_id, conversation_id, session_id_str, fallback)) {
      return MEMORY_EXTRACTION_BUSY;
   }

   /* Prepare context */
   extraction_context_t *ctx = calloc(1, sizeof(extraction_context_t));
   if (!ctx) {
      extraction_slot_release(user_id);
      return 1;
   }

   ctx->user_id = user_id;
   ctx->conversation_id = conversation_id;
   if (session_id_str) {
      safe_strscpy(ctx->session_id, session_id_str);
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

   /* The messages to extract, verified to belong to conversation_id (see
    * memory_extraction_build_input).  NULL: refused or out of memory; either
    * way nothing is extracted and memory_recovery picks up anything eligible. */
   int batch_outside = OUTSIDE_NONE;
   struct json_object *filtered = memory_extraction_build_input(user_id, conversation_id,
                                                                conversation_history, last_msg_id,
                                                                &batch_outside);
   ctx->outside = outside_combine(floor, batch_outside);
   ctx->checked_max_id = memory_extraction_history_max_id(conversation_history);
   if (!filtered) {
      free(ctx);
      extraction_slot_release(user_id);
      return 0;
   }
   /* Nothing the user said is new (only DAWN's own posts, say): mark it done
    * rather than send the model an empty conversation.  Only as far as the
    * history checked: a reply arriving since then is still to be read. */
   ctx->first_msg_id = memory_extraction_history_first_id(filtered);
   if (!memory_extraction_input_has_question(filtered)) {
      json_object_put(filtered);
      /* The rows passed may still have read outside text (DAWN's posts): the
       * floor takes it with the cursor. */
      const int outside = ctx->outside;
      free(ctx);
      const int64_t checked_id = memory_extraction_history_max_id(conversation_history);
      if (conversation_id > 0 && checked_id > last_msg_id &&
          memory_db_set_last_extracted(conversation_id, message_count, checked_id, outside) !=
              MEMORY_DB_SUCCESS) {
         OLOG_WARNING("memory_extraction: conv %lld: cursor not advanced",
                      (long long)conversation_id);
      }
      OLOG_INFO("memory_extraction: conv %lld: nothing the user said is new; skipped",
                (long long)conversation_id);
      extraction_slot_release(user_id);
      return 0;
   }

   /* memory_extraction_build_input strips the history; this is the backstop,
    * so no reasoning a vendor issued for itself reaches the extraction model. */
   struct json_object *safe = llm_history_strip_internal(filtered);
   json_object_put(filtered);
   if (!safe) {
      free(ctx);
      extraction_slot_release(user_id);
      return 1;
   }
   ctx->conversation_json = strdup(json_object_to_json_string_ext(safe, JSON_C_TO_STRING_PLAIN));
   json_object_put(safe);

   if (!ctx->conversation_json) {
      free(ctx);
      extraction_slot_release(user_id);
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
      extraction_slot_release(user_id);
      return 1;
   }

   OLOG_INFO("memory_extraction: triggered for user %d", user_id);
   return 0;
}

int memory_trigger_extraction(int user_id,
                              int64_t conversation_id,
                              const char *session_id_str,
                              struct json_object *conversation_history,
                              int message_count,
                              int duration_seconds,
                              const memory_extraction_fallback_t *fallback) {
   return trigger_extraction(user_id, conversation_id, session_id_str, conversation_history,
                             message_count, duration_seconds, fallback, false);
}

/* A queued conversation's run, on the slot claimed when it was taken
 * (memory_extraction_slots.c). */
int extraction_run_queued(int user_id,
                          int64_t conversation_id,
                          const char *session_id,
                          struct json_object *history,
                          int message_count,
                          const memory_extraction_fallback_t *fallback) {
   return trigger_extraction(user_id, conversation_id, session_id, history, message_count, 0,
                             fallback, true);
}

size_t memory_extraction_get_template_size_chars(void) {
   return strlen(MEMORY_EXTRACTION_PROMPT_TEMPLATE);
}

/* Set only by the benchmark harness (DAWN_EXTRACTION_PROMPT_FILE); the daemon
 * never sets it, so it always extracts with the compiled template. */
static const char *const *s_template_pieces;

void memory_extraction_set_template_pieces(
    const char *const pieces[MEMORY_EXTRACTION_TEMPLATE_PIECES]) {
   s_template_pieces = pieces;
}

char *memory_extraction_build_prompt(const char *anchor,
                                     const char *conversation,
                                     const char *profile,
                                     const char *expiry) {
   const char *const values[MEMORY_EXTRACTION_TEMPLATE_PIECES - 1] = { anchor, conversation,
                                                                       profile, expiry };
   size_t values_len = 0;
   for (int i = 0; i < MEMORY_EXTRACTION_TEMPLATE_PIECES - 1; i++)
      values_len += strlen(values[i]);

   if (s_template_pieces) {
      /* A benchmark's prompt file: its text goes in as text, never as a format. */
      size_t total = values_len + 1;
      for (int i = 0; i < MEMORY_EXTRACTION_TEMPLATE_PIECES; i++)
         total += strlen(s_template_pieces[i]);
      strbuf_t sb;
      strbuf_init_with_max(&sb, total, total);
      for (int i = 0; i < MEMORY_EXTRACTION_TEMPLATE_PIECES - 1; i++) {
         strbuf_append(&sb, s_template_pieces[i]);
         strbuf_append(&sb, values[i]);
      }
      strbuf_append(&sb, s_template_pieces[MEMORY_EXTRACTION_TEMPLATE_PIECES - 1]);
      return strbuf_steal(&sb);
   }

   /* The template's %s and %% shrink in the output, so its length covers them. */
   size_t size = strlen(MEMORY_EXTRACTION_PROMPT_TEMPLATE) + values_len + 1;
   char *prompt = malloc(size);
   if (prompt)
      snprintf(prompt, size, MEMORY_EXTRACTION_PROMPT_TEMPLATE, anchor, conversation, profile,
               expiry);
   return prompt;
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
      safe_strncpy(model_buf, model, model_buf_sz);
      cfg->model = model_buf;
   }

   if (strcmp(provider, "local") == 0 || strcmp(provider, "ollama") == 0) {
      cfg->type = LLM_LOCAL;
      cfg->cloud_provider = CLOUD_PROVIDER_NONE;
      safe_strncpy(endpoint_buf, g_config.llm.local.endpoint, endpoint_buf_sz);
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
   } else if (strcmp(provider, "openrouter") == 0) {
      cfg->type = LLM_CLOUD;
      cfg->cloud_provider = CLOUD_PROVIDER_OPENROUTER;
      cfg->api_key = g_secrets.openrouter_api_key;
      cfg->endpoint = OPENROUTER_URL;
      /* extraction_model is a "vendor/model" slug here; fall back to the main OpenRouter
       * default when unset. */
      if (!cfg->model || cfg->model[0] == '\0') {
         safe_strncpy(model_buf, llm_get_default_openrouter_model(), model_buf_sz);
         cfg->model = model_buf;
      }
   } else {
      OLOG_WARNING("%s: unknown provider '%s' in config "
                   "(valid: local, ollama, openai, claude, openrouter) - falling back to local",
                   prefix, provider);
      cfg->type = LLM_LOCAL;
      cfg->cloud_provider = CLOUD_PROVIDER_NONE;
      safe_strncpy(endpoint_buf, g_config.llm.local.endpoint, endpoint_buf_sz);
      cfg->endpoint = endpoint_buf;
   }

   /* Cloud calls require a key; fail early with a legible error rather than issuing a
    * null-key request that surfaces as a downstream 401 (P1 — same guard as
    * resolve_silent_observe_config). */
   if (cfg->type == LLM_CLOUD && (!cfg->api_key || cfg->api_key[0] == '\0')) {
      OLOG_ERROR("%s: cloud provider '%s' has no API key", prefix, provider);
      return FAILURE;
   }

   cfg->suppress_tools = true;
   safe_strscpy(cfg->thinking_mode, "disabled");
   cfg->timeout_ms = g_config.memory.extraction_timeout_ms;

   return SUCCESS;
}
