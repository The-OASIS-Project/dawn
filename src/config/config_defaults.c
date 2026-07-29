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
 * DAWN Configuration System - Default value initialization
 *
 * All default values match the compile-time constants in dawn.h and dawn.c
 */

#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "memory/memory_db.h"

/* =============================================================================
 * Global Configuration Instances
 * ============================================================================= */
dawn_config_t g_config;
secrets_config_t g_secrets;

/* =============================================================================
 * Helper Macros
 * ============================================================================= */
#define SAFE_COPY(dst, src)                   \
   do {                                       \
      strncpy((dst), (src), sizeof(dst) - 1); \
      (dst)[sizeof(dst) - 1] = '\0';          \
   } while (0)

/* =============================================================================
 * Default Values - matching dawn.h and dawn.c
 * ============================================================================= */
void config_set_defaults(dawn_config_t *config) {
   if (!config)
      return;

   memset(config, 0, sizeof(*config));

   /* General */
   SAFE_COPY(config->general.ai_name, "friday");
   config->general.log_file[0] = '\0'; /* Empty = stdout */

   /* Persona - empty means use compile-time default from dawn.h */
   config->persona.description[0] = '\0';

   /* Localization */
   config->localization.location[0] = '\0'; /* No default location */
   config->localization.timezone[0] = '\0'; /* System default */
   SAFE_COPY(config->localization.units, "imperial");

   /* Audio */
   SAFE_COPY(config->audio.backend, "auto");
   SAFE_COPY(config->audio.capture_device, "default");
   SAFE_COPY(config->audio.playback_device, "default");
   config->audio.output_rate = 44100; /* CD quality, native for most music */
   config->audio.output_channels = 2; /* Stereo required for dmix compatibility */

   /* Audio barge-in */
   config->audio.bargein.enabled = true;
   config->audio.bargein.cooldown_ms = 1500;        /* VAD_TTS_COOLDOWN_MS */
   config->audio.bargein.startup_cooldown_ms = 300; /* VAD_TTS_STARTUP_COOLDOWN_MS */

   /* Audio named devices - empty by default (optional, configured per-user) */
   config->audio.named_device_count = 0;

   /* VAD - matching defines in dawn.c */
   config->vad.speech_threshold = 0.5f;        /* VAD_SPEECH_THRESHOLD */
   config->vad.speech_threshold_tts = 0.92f;   /* VAD_SPEECH_THRESHOLD_TTS */
   config->vad.silence_threshold = 0.3f;       /* VAD_SILENCE_THRESHOLD */
   config->vad.end_of_speech_duration = 1.2f;  /* VAD_END_OF_SPEECH_DURATION */
   config->vad.max_recording_duration = 30.0f; /* VAD_MAX_RECORDING_DURATION */
   config->vad.preroll_ms = 500;

   /* VAD Chunking - matching defines in dawn.c */
   config->vad.chunking.enabled = true;
   config->vad.chunking.pause_duration = 0.3f; /* VAD_CHUNK_PAUSE_DURATION */
   config->vad.chunking.min_duration = 1.0f;   /* VAD_MIN_CHUNK_DURATION */
   config->vad.chunking.max_duration = 10.0f;  /* VAD_MAX_CHUNK_DURATION */

   /* ASR */
   SAFE_COPY(config->asr.model, "base.en");
   SAFE_COPY(config->asr.models_path, "models/whisper.cpp");
   config->asr.dedup_window_sec = ASR_DEDUP_WINDOW_SEC_DEFAULT;
   /* disambiguation_hint left empty by the memset above → built-in default
    * (DEFAULT_ASR_DISAMBIGUATION_HINT) is used at prompt-build time. */

   /* TTS */
   SAFE_COPY(config->tts.models_path, "models");
   SAFE_COPY(config->tts.voice_model, "en_GB-alba-medium");
   config->tts.length_scale = 0.85f;
   /* voice_directive / voice_directive_webui left empty by the memset above →
    * built-in defaults (DEFAULT_VOICE_OUTPUT_DIRECTIVE[_WEBUI]) are used. */

   /* Commands */
   SAFE_COPY(config->commands.processing_mode, "direct_first");

   /* LLM */
   SAFE_COPY(config->llm.type, "cloud");
   config->llm.max_tokens = 16384; /* Generous, universally-safe ceiling (= GPT-4o max output,
                                      well within Claude 4.x 64K); a ceiling, not a target */

   /* LLM Cloud */
   config->llm.cloud.provider[0] = '\0'; /* Empty = auto-detect from available API keys */
   config->llm.cloud.endpoint[0] = '\0'; /* Empty = use default */
   config->llm.cloud.vision_enabled = true;
   config->llm.cloud.use_openrouter = false; /* Direct providers by default */
   /* OpenAI endpoint selection: auto routes gpt-5.4* to /v1/responses */
   SAFE_COPY(config->llm.cloud.openai_use_responses_api, "auto");

   /* Default OpenAI model list (first entry is default).
    * gpt-5.4 family routes through /v1/responses; older entries use chat completions. */
   config->llm.cloud.openai_models_count = 7;
   SAFE_COPY(config->llm.cloud.openai_models[0], LLM_DEFAULT_OPENAI_MODEL); /* gpt-5.4 */
   SAFE_COPY(config->llm.cloud.openai_models[1], "gpt-5.4-mini");
   SAFE_COPY(config->llm.cloud.openai_models[2], "gpt-5.4-nano");
   SAFE_COPY(config->llm.cloud.openai_models[3], "gpt-5.2");
   SAFE_COPY(config->llm.cloud.openai_models[4], "gpt-5-mini");
   SAFE_COPY(config->llm.cloud.openai_models[5], "gpt-5-nano");
   SAFE_COPY(config->llm.cloud.openai_models[6], "o4-mini");
   config->llm.cloud.openai_default_model_idx = 0;

   /* Default Claude model list (first entry is default) */
   config->llm.cloud.claude_models_count = 3;
   SAFE_COPY(config->llm.cloud.claude_models[0], LLM_DEFAULT_CLAUDE_MODEL);
   SAFE_COPY(config->llm.cloud.claude_models[1], "claude-opus-4-6");
   SAFE_COPY(config->llm.cloud.claude_models[2], "claude-haiku-4-5");
   config->llm.cloud.claude_default_model_idx = 0;

   /* Default Gemini model list (first entry is default) */
   config->llm.cloud.gemini_models_count = 5;
   SAFE_COPY(config->llm.cloud.gemini_models[0], LLM_DEFAULT_GEMINI_MODEL);
   SAFE_COPY(config->llm.cloud.gemini_models[1], "gemini-2.5-pro");
   SAFE_COPY(config->llm.cloud.gemini_models[2], "gemini-2.5-flash-lite");
   SAFE_COPY(config->llm.cloud.gemini_models[3], "gemini-3-flash-preview");
   SAFE_COPY(config->llm.cloud.gemini_models[4], "gemini-3-pro-preview");
   config->llm.cloud.gemini_default_model_idx = 0;

   /* Default OpenRouter model list (curated favorites shown in the header switcher;
    * full live catalog browsing is Phase 2).  IDs are OpenRouter "vendor/model" slugs —
    * verify against https://openrouter.ai/models as the catalog shifts. */
   config->llm.cloud.openrouter_models_count = 8;
   SAFE_COPY(config->llm.cloud.openrouter_models[0], "anthropic/claude-opus-4.7");
   SAFE_COPY(config->llm.cloud.openrouter_models[1], "anthropic/claude-sonnet-4.6");
   SAFE_COPY(config->llm.cloud.openrouter_models[2], "anthropic/claude-haiku-4.5");
   SAFE_COPY(config->llm.cloud.openrouter_models[3], "openai/gpt-5.5");
   SAFE_COPY(config->llm.cloud.openrouter_models[4], "openai/gpt-5.4-mini");
   SAFE_COPY(config->llm.cloud.openrouter_models[5], "openai/gpt-5.4-nano");
   SAFE_COPY(config->llm.cloud.openrouter_models[6], "google/gemini-3.1-pro-preview");
   SAFE_COPY(config->llm.cloud.openrouter_models[7], "google/gemini-3.5-flash");
   config->llm.cloud.openrouter_default_model_idx = 1; /* anthropic/claude-sonnet-4.6 */

   /* LLM Local */
   SAFE_COPY(config->llm.local.endpoint, "http://127.0.0.1:8080");
   config->llm.local.model[0] = '\0';             /* Server decides */
   config->llm.local.vision_enabled = false;      /* Most local models don't support vision */
   SAFE_COPY(config->llm.local.provider, "auto"); /* Auto-detect Ollama vs llama.cpp */

   /* LLM Tools */
   SAFE_COPY(config->llm.tools.mode, "native"); /* "native", "command_tags", or "disabled" */

   /* LLM Silent-Observe (Phase 0 of Dynamic Context Injection)
    * Default to local provider so background observations don't accrue cloud
    * spend.  Operator can flip to a cloud provider with a configured key. */
   SAFE_COPY(config->llm.silent_observe.provider, "local");
   config->llm.silent_observe.model[0] = '\0';            /* Empty = let provider pick */
   config->llm.silent_observe.openrouter_model[0] = '\0'; /* Empty = main OpenRouter default */

   /* LLM Thinking/Reasoning */
   SAFE_COPY(config->llm.thinking.mode, "disabled");           /* "disabled", "enabled", "auto" */
   SAFE_COPY(config->llm.thinking.reasoning_effort, "medium"); /* Controls budget via dropdown */
   config->llm.thinking.budget_low = LLM_THINKING_BUDGET_LOW_DEFAULT;
   config->llm.thinking.budget_medium = LLM_THINKING_BUDGET_MEDIUM_DEFAULT;
   config->llm.thinking.budget_high = LLM_THINKING_BUDGET_HIGH_DEFAULT;
   config->llm.thinking.budget_xhigh = LLM_THINKING_BUDGET_XHIGH_DEFAULT;

   /* LLM Context Management */
   config->llm.summarize_threshold = 0.85f;        /* Legacy alias — maps to hard threshold */
   config->llm.compact_soft_threshold = 0.60f;     /* Async compaction trigger (background) */
   config->llm.compact_hard_threshold = 0.85f;     /* Blocking compaction trigger (safety net) */
   config->llm.compact_use_session = true;         /* Use session's provider for compaction */
   config->llm.compact_provider[0] = '\0';         /* Dedicated provider (empty = none) */
   config->llm.compact_model[0] = '\0';            /* Dedicated model (empty = none) */
   config->llm.compact_openrouter_model[0] = '\0'; /* Empty = main OpenRouter default */
   config->llm.conversation_logging = false; /* Disabled: WebUI saves to DB, set true for debug */
   config->llm.rate_limit_enabled = true;    /* Throttle cloud API calls by default */
   config->llm.rate_limit_rpm = 40;          /* 20% headroom under typical 50 RPM limit */

   /* Search */
   SAFE_COPY(config->search.engine, "searxng");
   SAFE_COPY(config->search.endpoint, "http://127.0.0.1:8384");

   /* Search Summarizer */
   SAFE_COPY(config->search.summarizer.backend, "tfidf"); /* Fast local extractive summarization */
   config->search.summarizer.threshold_bytes = 4096;
   config->search.summarizer.target_words = 600;
   config->search.summarizer.target_ratio = 0.2f; /* Keep 20% of sentences for TF-IDF */

   /* Search Title Filters - exclude low-quality SEO spam from news results */
   SAFE_COPY(config->search.title_filters[0], "wordle");
   SAFE_COPY(config->search.title_filters[1], "connections hints");
   SAFE_COPY(config->search.title_filters[2], "connections answers");
   SAFE_COPY(config->search.title_filters[3], "nyt connections");
   SAFE_COPY(config->search.title_filters[4], "puzzle hints");
   SAFE_COPY(config->search.title_filters[5], "puzzle answers");
   config->search.title_filters_count = 6;

   /* URL Fetcher - whitelist is zeroed by memset */
   config->url_fetcher.whitelist_count = 0;

   /* Fallback engine selection — preserves existing FlareSolverr default;
    * users opt into Tavily by changing this string. */
   SAFE_COPY(config->url_fetcher.fallback, "flaresolverr");

   /* FlareSolverr */
   config->url_fetcher.flaresolverr.enabled = false;
   SAFE_COPY(config->url_fetcher.flaresolverr.endpoint, "http://127.0.0.1:8191/v1");
   config->url_fetcher.flaresolverr.timeout_sec = 10;
   config->url_fetcher.flaresolverr.max_response_bytes = 4 * 1024 * 1024; /* 4MB */

   /* Tavily /extract (no enabled flag — selection is via url_fetcher.fallback) */
   config->url_fetcher.tavily.timeout_sec = 30;
   config->url_fetcher.tavily.max_response_bytes = 1 * 1024 * 1024; /* 1MB */
   SAFE_COPY(config->url_fetcher.tavily.extract_depth, "advanced");
   config->url_fetcher.tavily.rate_limit_per_minute = 10;
   config->url_fetcher.tavily.rate_limit_per_hour = 100;
   config->url_fetcher.tavily.rate_limit_per_day = 500;

   /* MQTT - matching dawn.h */
   config->mqtt.enabled = true;
   SAFE_COPY(config->mqtt.broker, "127.0.0.1"); /* MQTT_IP */
   config->mqtt.port = 1883;                    /* MQTT_PORT */
   config->mqtt.tls = false;
   config->mqtt.tls_ca_cert[0] = '\0';
   config->mqtt.tls_cert_path[0] = '\0';
   config->mqtt.tls_key_path[0] = '\0';

   /* Network */
   config->network.workers = 2;
   config->network.session_timeout_sec = 1800;         // 30 minutes
   config->network.llm_timeout_ms = 60000;             // 60 seconds for LLM requests
   config->network.summarization_timeout_ms = 180000;  // 3 minutes for context summarization

   /* TUI */
   config->tui.enabled = false;

   /* WebUI */
   config->webui.enabled = false;
   config->webui.port = 3000; /* "I love you 3000" */
   config->webui.max_clients = 4;
   config->webui.audio_chunk_ms = 100; /* 100ms chunks for lower VAD latency */
   SAFE_COPY(config->webui.www_path, "www");
   SAFE_COPY(config->webui.bind_address, "0.0.0.0");
   config->webui.https = false;
   config->webui.ssl_cert_path[0] = '\0';
   config->webui.ssl_key_path[0] = '\0';
   config->webui.export_max_messages = 5000;
   SAFE_COPY(config->webui.export_format, "json");

   /* Images - storage settings for vision uploads */
   config->images.retention_days = 0; /* 0 = never delete (user preference) */
   config->images.max_size_mb = 4;    /* 4MB max per image */
   config->images.max_per_user = 1000;
   config->images.cache_size_mb = 200; /* 200MB LRU cache cap */

   /* Documents - upload and extraction limits */
   config->documents.max_file_size_kb =
       10240; /* 10 MB — allow real PDFs (and their stored originals) */
   config->documents.max_documents = 5;
   config->documents.max_pages = 100;
   config->documents.max_extracted_size_kb = 1024;
   config->documents.max_index_size_kb = 2048;
   config->documents.max_indexed_documents = 50;
   /* v61 hybrid document search.  Reuse the memory subsystem's 0.3/0.7
    * keyword/vector magnitudes for cross-subsystem consistency. */
   config->documents.fts_label_weight = 3.0f;
   config->documents.fts_body_weight = 1.0f;
   config->documents.hybrid_keyword_weight = 0.3f;
   config->documents.hybrid_vector_weight = 0.7f;
   config->documents.phrase_bonus_weight = 0.25f;
   config->documents.search_min_score = 0.3f;
   config->documents.version_retention_days = 14; /* v62 note/doc undo window */
   config->documents.version_keep_per_doc = 10;
   /* v68 original-file storage — keep-forever by default, orphan sweep only. */
   config->documents.originals_enabled = true;
   config->documents.original_retention_days = 0; /* 0 = keep forever */
   config->documents.max_original_size_mb = 10;
   config->documents.max_originals_per_user = 200;
   config->documents.max_originals_total_mb_per_user = 2048;
   config->documents.original_grace_minutes = 45;

   /* Vision - per-upload image size and dimension limits */
   config->vision.max_image_size_kb = 4096;
   config->vision.max_dimension = 1024;
   config->vision.max_images = 5;
   config->vision.capture_history_count = 1;

   /* Memory - persistent user memory system */
   config->memory.enabled = true;
   config->memory.context_budget_tokens = 800; /* ~3200 chars for memory context */
   /* Default = 3072 (~768 tokens of verbatim source per memory tool call).
    * See dawn_config.h source_budget_chars for the bench-validated lift table. */
   config->memory.source_budget_chars = 3072;
   SAFE_COPY(config->memory.extraction_provider, "local");
   SAFE_COPY(config->memory.extraction_model, "qwen2.5:7b");
   config->memory.extraction_openrouter_model[0] = '\0'; /* Empty = main OpenRouter default */
   config->memory.extraction_timeout_ms = 120000;        /* 2 minutes for fact extraction */
   config->memory.pruning_enabled = true;
   config->memory.prune_superseded_days = 30; /* Delete old superseded facts after 30 days */
   config->memory.prune_stale_days = 180; /* Delete unused low-confidence facts after 6 months */
   config->memory.prune_stale_min_confidence = 0.5f; /* Only prune facts below 50% confidence */
   config->memory.expire_enabled = false;            /* C3: off until recall-validated */
   config->memory.expire_grace_days = 7;             /* Visible 1 week past the reference date */
   config->memory.prune_expired_days = 30;           /* Recoverable buffer before hard delete */
   config->memory.conversation_idle_timeout_min =
       15;                                   /* Auto-save voice conversations after 15 min */
   config->memory.default_voice_user_id = 1; /* Assign to first user (admin) by default */

   /* Memory decay (Phase 5) */
   config->memory.decay_enabled = true;
   config->memory.decay_hour = 2;                     /* Run at 2 AM */
   config->memory.decay_inferred_weekly = 0.95f;      /* Inferred facts lose 5%/week */
   config->memory.decay_explicit_weekly = 0.98f;      /* Explicit facts lose 2%/week */
   config->memory.decay_preference_weekly = 0.97f;    /* Preferences lose 3%/week */
   config->memory.decay_inferred_floor = 0.0f;        /* Inferred can decay to zero */
   config->memory.decay_explicit_floor = 0.50f;       /* Explicit never below 50% */
   config->memory.decay_preference_floor = 0.40f;     /* Preferences never below 40% */
   config->memory.decay_prune_threshold = 0.25f;      /* Prune facts below 25% */
   config->memory.summary_retention_days = 30;        /* Delete summaries after 30 days */
   config->memory.access_reinforcement_boost = 0.05f; /* +5% on access */

   /* Memory embeddings (semantic search) */
   SAFE_COPY(config->memory.embedding_provider, "onnx");
   /* embedding_model, embedding_endpoint: empty (memset zero) — ONNX needs neither */
   config->memory.embedding_keyword_weight = 0.3f;
   config->memory.embedding_vector_weight = 0.7f;
   config->memory.embedding_backfill_on_startup = true;
   SAFE_COPY(config->memory.model_id, "bge-small-en-v1.5-int8");
   config->memory.recompute_on_model_change = true;
   config->memory.recompute_batch_size = 50;
   config->memory.recompute_batch_sleep_ms = 100;
   /* Temporal scoring: +2.3pp R@10 / +0.8pp R@5 on LongMemEval temporal-reasoning
    * at weight=0.20; plateau at 0.30. Zero cost on queries without temporal
    * expressions (parser returns "not found", no boost applied).  Safe default. */
   config->memory.temporal_weight = 0.20f;
   /* Hard temporal filter on memory.search results.  Graduated to default-on
    * (May 2026) after +3.6pp LongMemEval temporal-reasoning validation.
    * LoCoMo-blind (parser doesn't fire on cat-2 "what date" shape) but no
    * regression observed; complements (does not replace) soft temporal_weight. */
   config->memory.temporal_filter_enabled = true;
   /* RRF retrieval — formally dead-letter (May 17, 2026).  Original 2026-05-15
    * bench showed -3.7pp regression vs composite scorer.  Re-tested post Phase 1
    * BM25 + Phase 3 specificity rules (Phase 4A of Mem0 parity program): still
    * -0.58pp regressive.  Flag kept off-by-default; code stays behind the flag
    * for operator override but is no longer "experimental, may re-enable" —
    * the parallel-channel RRF design does not compose with DAWN's entity-graph
    * backbone.  See docs/MEM0_ARCHITECTURAL_PARITY.md §Phase 4A outcome. */
   config->memory.rrf_enabled = false;
   /* Minimal memory_text format — graduated to default-on (May 2026) after
    * +0.98pp leader-comparable validation on n=1525.  Re-confirmed post
    * Phase 3 specificity (Phase 4B of Mem0 parity): -0.71pp without minimal.
    * Cuts ~30-50% of memory_text bytes by stripping per-fact metadata +
    * demoting source excerpts to a single global block. */
   config->memory.memory_text_minimal = true;
   /* BM25 keyword retrieval (FTS5 + Porter2).  Graduated to default-on
    * (May 17, 2026) after Phase 1 Mem0 parity bench validation: +2.74pp
    * leader-comparable on fresh LoCoMo n=1536 (BM25 OFF 0.7057 → ON 0.7331),
    * plus stable burn-in on the dev's live deployment.  Operator override
    * via [memory.embeddings] bm25_enabled = false to revert to the legacy
    * LIKE + multi-token match-count path.  See NOTICE for Mem0 attribution
    * and docs/MEM0_ARCHITECTURAL_PARITY.md §Phase 1 outcome. */
   config->memory.bm25_enabled = true;
   config->memory.category_threshold = 0.25f;
   /* Graph-retrieval Phase 2.b — entity-graph candidate source.  Default-on;
    * entity_grounding_bonus 0.4 sits between the 0.30 search floor and
    * typical strong hybrid hits; max_facts_per_query 30 bounds fan-out for
    * high-degree entities. */
   config->memory.graph_retrieval.enabled = true;
   config->memory.graph_retrieval.entity_grounding_bonus = 0.40f;
   config->memory.graph_retrieval.max_facts_per_query = 30;
   /* Phase 2 Step 1: query-aware scoring of graph candidates.
    * Default ON — replaces the flat entity_grounding_bonus with
    * vec_weight * cosine(query, fact) + entity_bonus.  Toggle false
    * for ablation. */
   config->memory.graph_retrieval.use_query_scoring = true;
   /* entity_bonus default 0.0 (May 14, 2026 architecture review):
    * applied asymmetrically (only to facts arriving via the graph
    * branch) it acted as an unearned thumb-on-the-scale that promoted
    * entity-mentioning facts over equally-relevant facts that just
    * didn't lexically name the seed entity.  Cat-3 ("inference")
    * questions are exactly the case where the answer often doesn't
    * repeat the seed, so the bonus regressed cat-3 by -10pp.  Keep
    * the knob; default 0.0 means "use cosine alone for scoring," i.e.
    * symmetric with hybrid candidates that share the same vec_weight
    * scale.  Set >0 only as an experimental knob to test whether
    * boosting graph candidates symmetrically (require Phase 1B
    * patch first) adds value. */
   config->memory.graph_retrieval.entity_bonus = 0.0f;

   /* Memory-tool search score floor: drops marginal-cosine hits before they
    * reach the LLM.  0.30 sits at the natural kw_weight boundary — pure
    * single-token keyword matches score exactly `kw_weight * 1.0 = 0.30`,
    * so 0.30 is the cleanest cutoff that filters hybrid-vector-only
    * marginal hits without dropping any legitimate keyword match.  Live-
    * validated 2026-05-13 across three settings (0.10 / 0.20 / 0.30) on
    * a no-memory query: 0.30 was the only setting that produced visible
    * filtering AND surfaced focus-block candidates the LLM could use for
    * intelligent clarifying questions.  See dawn_config.h
    * §search_score_floor. */
   config->memory.search_score_floor = 0.30f;
   /* Paraphrase-dedup gate: 0.92 is conservative — calibrated for bge-small-
    * en-v1.5-int8 to favor precision over recall (false merges cost more
    * than false misses).  Tune via benchmarks/bench_paraphrase_calibration.py
    * if the embedder is swapped. */
   config->memory.paraphrase_dedup_enabled = true;
   config->memory.paraphrase_dedup_threshold = MEMORY_PARAPHRASE_DEDUP_DEFAULT;

   /* Note-extraction guard: keep filed reference text out of semantic facts. */
   config->memory.note_extraction_guard = true;

   /* Extraction recovery: re-extract conversations stuck idle past the threshold. */
   config->memory.recovery_enabled = true;
   config->memory.recovery_idle_threshold_seconds = 3600; /* 1 hour */
   config->memory.recovery_max_attempts = 2;
   config->memory.recovery_recurring_interval_seconds = 86400; /* daily */

   /* Per-turn focus injection (Phase 1 of Dynamic Context Injection) —
    * disabled by default until adapters land in 1c/1d and the
    * prompt-builder integration ships in 1e.  Defaults match the design
    * doc §"Phase 1 — Per-Turn Focus" TOML block. */
   config->memory.focus_injection.enabled = false;
   /* Per-turn focus-block budget in BYTES (same unit as FOCUS_TEXT_MAX_BYTES).
    * 10240 ≈ two full document chunks plus several small fact/summary
    * candidates — the multi-source coexistence target. */
   config->memory.focus_injection.focus_budget_bytes = 10240;
   /* top_k bumped 8 → 12 (May 2026, post-Step-3 semantic summary adapter).
    * With summaries newly competing for the per-turn budget, top_k=8 was
    * filled by facts alone and starved every other source.  Live testing
    * on a 271-summary corpus shows 12 lets memory_summary + memory_entity
    * + memory_relation all surface alongside facts.  Re-bench when Phase
    * 1j fixtures get summary-relevant probes. */
   config->memory.focus_injection.top_k = 12;
   config->memory.focus_injection.min_score = 0.4f;
   config->memory.focus_injection.classifier_enabled = false;
   config->memory.focus_injection.summary_max_scan = MEMORY_SUMMARY_SEMANTIC_SCAN_CAP_DEFAULT;
   /* Phase 1j tuning (May 2026): w_imp 0.2 → 1.0, w_rec 0.3 → 0.15.
    * Probe Component 6 fix-rate lifted 7/11 → 11/11 across all three
    * providers (anthropic / openai / local) on the v2 fixtures.  See
    * atlas/dawn/memory/PHASE_1J_TUNING_LOG.md for rationale and the
    * bench-validated pathologies these weights resolve.
    *
    * Phase 1j re-bench (2026-05-13): w_rec 0.15 → 0.30 after the
    * post-Step-4 corpus exposed a paraphrase-ladder pathology the
    * original 11 fixtures didn't probe.  4 new cases (12-15 in
    * benchmarks/focus_probe_cases.json) probe summary-vs-ladder
    * recency competition + regression-guard against over-correction;
    * Run B at w_rec=0.30 produced 15/15 across all three providers
    * with no regression on existing cases or the regression guard.
    * The candidate weight is now the production default.  Run A at
    * w_rec=0.15 produced 13/15 (cases 12, 13 failed FAIL → FAIL as
    * designed). */
   config->memory.focus_injection.weight_semantic = 1.0f;
   config->memory.focus_injection.weight_recency = 0.30f;
   config->memory.focus_injection.weight_importance = 1.0f;
   config->memory.focus_injection.weight_source = 1.0f;
   config->memory.focus_injection.source_weights.memory_fact = 1.0f;
   config->memory.focus_injection.source_weights.memory_entity = 0.9f;
   config->memory.focus_injection.source_weights.memory_relation = 0.85f;
   /* memory_summary bumped 0.7 → 1.0 (May 2026, post-Step-3).  The 0.7
    * value was a first-cut guess from before the semantic summary adapter
    * landed; at 0.7 summaries lost the ranking competition to facts on
    * every turn even when cosine was strong, so they never surfaced in
    * the per-turn injection.  Parity with memory_fact (1.0) lets them
    * compete on raw signal quality.  Re-bench when Phase 1j gains a
    * summary-relevant probe. */
   config->memory.focus_injection.source_weights.memory_summary = 1.0f;
   config->memory.focus_injection.source_weights.document_chunk = 0.7f;
   config->memory.focus_injection.source_weights.calendar_event = 0.6f;
   config->memory.focus_injection.source_weights.recent_email = 0.5f;
   config->memory.focus_injection.source_weights.dawn_background = 0.8f;
   config->memory.focus_injection.dedup.recent_window_turns = 8;
   config->memory.focus_injection.dedup.score_uplift_factor = 1.5f;

   /* Dominant-token over-inclusion heuristic (Phase B reranker workstream,
    * 2026-05-13).  Bench-validated at canonical params on focus_probe_cases.json
    * cases 16-18: 18/18 quorum across all three providers, parameter-robust
    * across threshold ∈ {0.50, 0.60, 0.70} × base_penalty ∈ {0.30, 0.40,
    * 0.50}.  See docs/RERANKER_PHASE_A_DESIGN.md for the full bench
    * methodology and decision rationale.  Default-on because the bench
    * shows zero regression on existing 15 cases and clean rescue of the
    * three severe-pathology probes. */
   config->memory.focus_injection.dominant_token_heuristic.enabled = true;
   config->memory.focus_injection.dominant_token_heuristic.threshold = 0.60f;
   config->memory.focus_injection.dominant_token_heuristic.base_penalty = 0.40f;

   /* Unified cross-source `recall` tool (docs/CROSS_TOOL_RECALL_DESIGN.md §4.5).
    * Deep gather: bigger than per-turn (top_k 12 / 10240 / 0.4) but bounded —
    * 24 KB ≈ 6 K tokens fed back, deliberately NOT the 64 KB cap (no reranker to
    * defend a long tail).  min_score lower so weaker-but-relevant hits surface.
    * per_source_max 16 keeps any one source from crowding out the rest; at the
    * current 6 live adapters that is a 96-candidate pool, and 16*MAX_FOCUS_SOURCES
    * (16) = 256 keeps the dominant-token heuristic alive even at a full registry
    * (see the validation cap in config_validate.c). */
   config->memory.recall.top_k = 40;
   config->memory.recall.budget_bytes = 24576;
   config->memory.recall.min_score = 0.25f;
   config->memory.recall.per_source_max = 16;

   /* Phase 2 entity-merge auto-merge gate.  auto_threshold=0.90 stays
    * conservative because auto-merges land with no human approval.
    * review_threshold=0.50 is permissive — proposals go through the
    * Suggested-Merges WebUI panel where the operator gates the actual
    * write.  Lower bound on legitimate matches: substring (+0.10) +
    * type_match (+0.05) + cosine(0.7) (+0.21) + jaccard(0.5) (+0.15)
    * = 0.51, just above the floor.  Random pairs without substring/
    * jaccard stay below.  Calibrate against the dev corpus by watching
    * the WebUI proposal queue for false-positive rate. */
   config->memory.entity_merge_enabled = true;
   config->memory.entity_merge_auto_threshold = 0.90f;
   config->memory.entity_merge_review_threshold = 0.50f;

   /* Shutdown - disabled by default for security */
   config->shutdown.enabled = false;
   config->shutdown.passphrase[0] = '\0';

   /* Debug */
   config->debug.mic_record = false;
   config->debug.asr_record = false;
   config->debug.aec_record = false;
   SAFE_COPY(config->debug.record_path, "/tmp");
   config->debug.silent_observe_test_endpoint = false; /* Off by default — dev only */

   /* Paths */
   SAFE_COPY(config->paths.data_dir, "~/.local/share/dawn"); /* Database storage directory */
   SAFE_COPY(config->paths.music_dir, "~/Music");

   /* Music - metadata indexing */
   config->music.scan_interval_minutes = 60; /* Rescan every hour */

   /* Music - Plex Media Server defaults */
   config->music.plex.host[0] = '\0';       /* Must be configured by user */
   config->music.plex.port = 32400;         /* Standard Plex port */
   config->music.plex.music_section_id = 0; /* 0 = auto-discover */
   config->music.plex.ssl = false;
   config->music.plex.ssl_verify = true;
   config->music.plex.client_identifier[0] = '\0'; /* Auto-generated on first use */

   /* Scheduler */
   config->scheduler.enabled = true;
   config->scheduler.default_snooze_minutes = 10;
   config->scheduler.max_snooze_count = 10;
   config->scheduler.max_events_per_user = 50;
   config->scheduler.max_events_total = 200;
   config->scheduler.missed_event_recovery = true;
   SAFE_COPY(config->scheduler.missed_task_policy, "skip");
   config->scheduler.missed_task_max_age_sec = 300;
   config->scheduler.alarm_timeout_sec = 60;
   config->scheduler.alarm_volume = 80;
   config->scheduler.event_retention_days = 30;
   config->scheduler.briefing_speak_aloud_on_webui_source = false;

   /* Background jobs — enabled by default; caps are LLM-resource-bound + DoS
    * guards (see docs/BACKGROUND_JOBS_DESIGN.md §7). */
   config->jobs.enabled = true;
   config->jobs.max_concurrent_local = 1;
   config->jobs.max_concurrent_cloud = 4;
   config->jobs.max_active_jobs = 16;
   config->jobs.max_jobs_per_user = 4;
   config->jobs.max_queued_per_user = 8;
   config->jobs.monitor_followups_per_tick = 4;
   config->jobs.max_spawn_depth = 3;
   config->jobs.max_children_per_tree = 4;
   config->jobs.max_reinvokes_per_tree = 8;
   config->jobs.max_concurrent_reinvokes = 2;
   config->jobs.max_runtime_sec = 1800;
   config->jobs.event_chunk_cap = 16384;
   config->jobs.event_retention_days = 30;

   /* Proactive attention (SAGE) — master switch OFF by default (opt-in). Watch
    * rules are DB-backed, not config. Numeric defaults mirror the module's
    * SAGE_MAX_ALERTS_PER_HOUR / judge floor. */
   config->attention.enabled = false;
   config->attention.max_alerts_per_hour = 4;
   config->attention.inject_into_sessions = false;
   config->attention.judge_enabled = false;
   config->attention.judge_threshold = 3.5;

   /* Calendar */
   config->calendar.enabled = true;
   config->calendar.sync_interval_sec = 900;
   config->calendar.cache_past_days = 30;
   config->calendar.cache_future_days = 365;
   config->calendar.default_event_duration_min = 60;

   /* Messaging channels */
   config->messaging.sms_active_window_sec = 600; /* 10 minutes */

   /* OTA (over-the-air satellite updates) — opt-in; serve disabled by default */
   config->ota.enabled = false;
   SAFE_COPY(config->ota.release_dir, "/var/lib/dawn/ota");
   config->ota.download_token_ttl_sec = 120; /* 2 min: enough to start the pull */
   config->ota.require_tls = true;           /* never serve OTA over plaintext */

   /* Code projects (coding harness) — disabled by default; secure-by-default caps. */
   SAFE_COPY(config->code_projects.source_root, "/var/lib/dawn/source");
   SAFE_COPY(config->code_projects.default_index_mode, "full");
   SAFE_COPY(config->code_projects.import_user_required, "admin");
   config->code_projects.max_repo_size_mb = 2048;
   config->code_projects.max_file_count = 200000;
   config->code_projects.max_path_depth = 20;
   config->code_projects.clone_depth = 0;
   SAFE_COPY(config->code_projects.allowed_host_pattern,
             "^([a-z0-9-]+\\.)?(github\\.com|gitlab\\.com|codeberg\\.org|bitbucket\\.org)$");

   /* Music - streaming settings */
   config->music.streaming_enabled = true;
   strncpy(config->music.streaming_quality, "standard",
           sizeof(config->music.streaming_quality) - 1);
   strncpy(config->music.streaming_bitrate_mode, "vbr",
           sizeof(config->music.streaming_bitrate_mode) - 1);
}

void config_set_secrets_defaults(secrets_config_t *secrets) {
   if (!secrets)
      return;

   memset(secrets, 0, sizeof(*secrets));
   /* All secrets default to empty strings */
}

const dawn_config_t *config_get(void) {
   return &g_config;
}

const secrets_config_t *config_get_secrets(void) {
   return &g_secrets;
}

void config_cleanup(void) {
   /* Currently a no-op - all config uses static allocation */
   /* Reserved for future use if dynamic resources are added */
   (void)0;
}
