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
 * Session manager for multi-client support.
 * Manages per-client conversation context with reference counting.
 */

#ifndef SESSION_MANAGER_H
#define SESSION_MANAGER_H

#include <arpa/inet.h>
#include <json-c/json.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h> /* free() in static-inline composed_prompt_free fallback */
#include <time.h>

#include "core/text_filter.h"   // For cmd_tag_filter_state_t
#include "llm/llm_interface.h"  // For session_llm_config_t

#define SESSION_PROVIDER_MAX 16
#define SESSION_MAX_PROVIDERS 4

/**
 * @brief Per-provider token tracking for a session
 */
typedef struct {
   char provider[SESSION_PROVIDER_MAX];  // "openai", "claude", "local"
   uint64_t tokens_input;
   uint64_t tokens_output;
   uint64_t tokens_cached;
   uint32_t queries;
} session_provider_tokens_t;

/**
 * @brief Per-session metrics tracker
 *
 * Tracks metrics during session lifetime. Saved to database after each query
 * using UPSERT pattern (INSERT first query, UPDATE thereafter).
 */
typedef struct {
   int64_t db_id;  // Database row ID (-1 = not yet saved)
   int user_id;    // User ID (0 = anonymous/local)

   // Query counts
   uint32_t queries_total;
   uint32_t queries_cloud;
   uint32_t queries_local;
   uint32_t errors_count;
   uint32_t fallbacks_count;

   // Per-provider token tracking
   session_provider_tokens_t providers[SESSION_MAX_PROVIDERS];
   int provider_count;

   // Performance tracking (sums + counts for computing averages)
   double asr_ms_sum;
   double llm_ttft_ms_sum;
   double llm_total_ms_sum;
   double tts_ms_sum;
   double pipeline_ms_sum;
   uint32_t perf_sample_count;  // Number of samples for averaging
} session_metrics_tracker_t;

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SESSIONS 8
#define SESSION_TIMEOUT_SEC 1800  // 30 minute idle timeout
#define LOCAL_SESSION_ID 0        // Reserved for local microphone

/**
 * LOCK ACQUISITION ORDER (to prevent deadlocks):
 *
 * Level 1: session_manager_rwlock (module-level, read or write)
 * Level 2: session->ref_mutex (per-session, protects ref_count)
 * Level 3: session->fd_mutex (per-session, protects client_fd during reconnect)
 * Level 4: session->llm_config_mutex, session->history_mutex, session->metrics_mutex,
 *          or session->tools_mutex
 *          (per-session leaf locks, never held together - copy-under-mutex pattern)
 *
 * CRITICAL: NEVER acquire locks in reverse order
 * CRITICAL: NEVER hold session_manager_rwlock when acquiring per-session locks
 *           for extended operations (brief hold for slot operations is OK)
 * CRITICAL: NEVER hold multiple Level 4 locks simultaneously
 *
 * External locks (tts_mutex, mqtt_mutex) are leaf locks and should only be
 * acquired when no session_manager locks are held.
 */

/**
 * @brief Session type enumeration
 */
typedef enum {
   SESSION_TYPE_LOCAL,      // Local microphone
   SESSION_TYPE_DAP,        // ESP32 satellite (DAP protocol)
   SESSION_TYPE_DAP2,       // DAP 2.0 satellite (Tier 1 or Tier 2)
   SESSION_TYPE_WEBUI,      // WebUI browser client
   SESSION_TYPE_MESSAGING,  // Messaging channel (Telegram / Discord / Slack / SMS).
                            // Owned by the messaging engine's slot map.  Exempt
                            // from session_cleanup_expired (engine handles
                            // lifetime via LRU eviction + /new reset).
} session_type_t;

/**
 * @brief DAP2 satellite tier (see DAP2_DESIGN.md Section 4)
 */
typedef enum {
   DAP2_TIER_1 = 1,  // Full satellite (RPi) - sends TEXT, receives TEXT
   DAP2_TIER_2 = 2,  // Audio satellite (ESP32) - sends ADPCM, receives ADPCM
} dap2_tier_t;

/**
 * @brief DAP2 satellite identity (from REGISTER message)
 */
typedef struct {
   char uuid[37];              // UUID string (e.g., "550e8400-e29b-41d4-a716-446655440000")
   char name[64];              // Human-readable name (e.g., "Kitchen Assistant")
   char location[32];          // Room/area (e.g., "kitchen") - used for context
   char hardware_id[64];       // Optional hardware serial
   char reconnect_secret[65];  // 32 bytes hex-encoded (prevents session hijacking)
} dap2_identity_t;

/**
 * @brief DAP2 satellite capabilities
 */
typedef struct {
   bool local_asr;  // Satellite can transcribe locally
   bool local_tts;  // Satellite can synthesize locally
   bool wake_word;  // Satellite has wake word detection
   bool ota;        // Satellite supports server-driven OTA updates (offer gating)
} dap2_capabilities_t;

/**
 * @brief Messaging-channel identity (for SESSION_TYPE_MESSAGING sessions).
 *
 * Populated by the messaging engine at session creation from the
 * messaging_channels row that matched the inbound's (provider,
 * provider_address).  Used by the prompt build path to surface
 * "you are responding through this channel" context to the LLM,
 * mirroring how dap2_identity_t.location surfaces Room.
 *
 * Empty `provider[0]` means "not a messaging session" — the prompt
 * helper guards on this.  channel_name is the user-facing display_name
 * (e.g., "slack_main", "telegram_personal") that the messaging tool
 * also accepts, so the LLM can self-reference it without translation.
 *
 * channel_name is REFRESHED from the DB on every turn (in
 * get_or_create_messaging_session, on the worker thread), not just at
 * session creation — so a mid-session channel rename is reflected in the
 * next prompt's current-channel injection and the LLM emits the correct
 * scheduler `deliver_to`.  Treat it as "current display_name as of this
 * turn," not a creation-time snapshot.
 */
typedef struct {
   char provider[16];        // "slack" / "telegram" / "discord" / "sms"
   char channel_name[64];    // current display_name from messaging_channels (refreshed per turn)
   int64_t conversation_id;  // forever-conversation row backing this channel (0 = none).
                             // Lets switch_llm persist a per-conversation LLM change to the
                             // conversations.llm_* columns, since session_t doesn't otherwise
                             // track its conv_id.
} messaging_identity_t;

/**
 * @brief Async compaction state machine (per-session, embedded in session_t)
 *
 * State transitions: IDLE -> RUNNING (trigger) -> READY (bg complete) -> IDLE (merge)
 * Single-writer discipline: bg thread writes result fields while RUNNING,
 * main thread reads them when READY. _Atomic state governs transitions.
 */
typedef enum {
   ASYNC_COMPACT_IDLE = 0,
   ASYNC_COMPACT_RUNNING = 1,
   ASYNC_COMPACT_READY = 2,
} async_compact_state_t;

/* =============================================================================
 * Phase 1f: per-turn focus-injection dedup state
 *
 * Tracks (source_id, item_id) tuples that have already been injected during
 * this session so build_focus_block can suppress repeats.  Lives in
 * session_t and shares the session's existing `history_mutex` — no new
 * lock, no new lock-ordering rule.
 *
 * Persistence policy: acceptable to lose on crash.  Sessions don't survive
 * crashes anyway, so a fresh in-memory state on the next start is correct.
 *
 * Hard cap with LRU eviction by `last_injected_turn` — entries[] is a
 * fixed-size array iterated linearly (N=256 is small enough that a hash
 * is premature optimization here).
 * ============================================================================= */

#define MAX_INJECTED_SET_SIZE 256

/**
 * @brief One tracked injection in the dedup set.
 *
 * `source_id` and `item_id` together form the dedup key.  `item_id` is
 * opaque server-generated content (per the focus-source framework's
 * contract — never user-controlled).
 */
typedef struct {
   char source_id[32]; /* Adapter-registered source string (memory_fact, ...) */
   char item_id[64];   /* Opaque server-generated item key — matches FOCUS_ITEM_ID_BUFLEN
                          from focus_candidate_helpers.h.  Static literal here
                          because session_manager.h cannot include L2 headers. */
   int first_injected_turn;
   int last_injected_turn;
   float last_score; /* Final ranked score from focus_compose; float to
                        match focus_candidate_t scoring fields and avoid
                        implicit double-conversion in uplift compare. */
} injected_set_entry_t;

/**
 * @brief Per-session dedup set + monotonic turn counter.
 *
 * Zeroed on session create (calloc) and on session_injected_set_clear().
 * The two `*_logged_once` flags gate per-session OLOG_INFO / OLOG_WARNING
 * lines so a long conversation produces exactly one such line per
 * condition rather than a stream.
 */
typedef struct {
   injected_set_entry_t entries[MAX_INJECTED_SET_SIZE];
   int count;
   int turn_counter;                /* Monotonic; advances per PER_TURN; resets on clear */
   bool eviction_logged_once;       /* Gate: first LRU eviction in this session */
   bool all_suppressed_logged_once; /* Gate: first all-candidates-suppressed
                                       turn in this session (observability for
                                       runaway suppression) */
} injected_set_t;

typedef struct {
   _Atomic int state;
   pthread_t thread_id;
   bool thread_active;

   struct json_object *pending_history;
   int result_tokens_before;
   int result_tokens_after;
   int result_messages_summarized;
   int result_level;
   char *result_summary;

   struct json_object *snapshot_history;
   int snapshot_msg_count;

   llm_type_t trigger_llm_type;
   cloud_provider_t trigger_cloud_provider;
   char trigger_model[64];
   uint32_t trigger_session_id;
   int64_t trigger_conv_id;

   time_t last_compacted_at;
} async_compaction_t;

/**
 * @brief Tool-turn persist callback (E2 structured tool-call persistence).
 *
 * Fired by the LLM tool loop once per appended tool message, in OpenAI-canonical
 * form. @p tool_calls_json is the assistant's tool_calls JSON array (NULL for
 * non-assistant rows); @p tool_call_id is the matching id on role="tool" rows
 * (NULL otherwise); @p reasoning_json is display-only reasoning JSON on the assistant
 * tool_calls row (NULL otherwise, and never read into the LLM context). The
 * implementation persists to conv_db.
 */
typedef void (*session_tool_persist_fn)(void *userdata,
                                        const char *role,
                                        const char *content,
                                        const char *tool_calls_json,
                                        const char *tool_call_id,
                                        const char *reasoning_json);

/**
 * @brief Tool-loop iteration-boundary callback.
 *
 * Fired by the LLM tool loop after an iteration produces tool calls (just before
 * the tools execute), so the WebUI can close the current streaming bubble — the next
 * iteration's text then opens a fresh bubble below the tool entries, matching the
 * reloaded-conversation order.  Opt-in per session: only installed by the WebUI text
 * path, so satellite / local-mic turns (which never set it) are unaffected.
 */
struct session; /* defined below; forward-declared for the callback typedef */
typedef void (*session_tool_iteration_fn)(struct session *session, void *userdata);

/**
 * @brief Session structure
 * @ownership Session manager owns all sessions
 * @thread_safety Protected by session_manager_rwlock
 */
typedef struct session {
   uint32_t session_id;
   session_type_t type;
   time_t created_at;
   time_t last_activity;
   time_t last_interaction_complete;  // Last wake-word-to-response cycle (for idle timeout)

   // Conversation history (owned by session, protected by history_mutex)
   struct json_object *conversation_history;
   pthread_mutex_t history_mutex;

   // Client-specific data
   int client_fd;                    // Socket for network clients (-1 for local)
   pthread_mutex_t fd_mutex;         // Protects client_fd during reconnection
   void *client_data;                // Type-specific data (WebSocket state, etc.)
   char client_ip[INET_ADDRSTRLEN];  // Client IP for DAP1 session persistence

   // DAP2-specific fields (only valid when type == SESSION_TYPE_DAP2)
   dap2_tier_t tier;                  // Tier 1 (text) or Tier 2 (audio)
   dap2_identity_t identity;          // UUID, name, location
   dap2_capabilities_t capabilities;  // Local ASR/TTS/wake word

   // Messaging-specific fields (only valid when type == SESSION_TYPE_MESSAGING).
   // Populated by the messaging engine at session creation from the
   // messaging_channels row that matched the inbound's (provider,
   // provider_address).  Empty provider[0] means "not a messaging session".
   messaging_identity_t messaging_identity;

   // Cancellation (atomic for cross-thread visibility on ARM64)
   atomic_bool disconnected;  // Set on client disconnect

   /* WebUI: was THIS turn's input voice (ASR-transcribed) vs typed?  Stamped on
    * the WORKER thread immediately before the synchronous prompt build — from the
    * text_work_t item for the text/always-on path, and directly before
    * session_dispatch_user_turn for the push-to-talk audio path — so it is
    * authoritative for the turn at build time (same discipline as the tts_enabled
    * read).  Read by the prompt builder to gate the ASR-disambiguation hint.  Only
    * meaningful for SESSION_TYPE_WEBUI (satellites/local are always voice-input).
    * Atomic for ARM64 visibility across worker threads. */
   atomic_bool input_was_voice;

   /* Idle-timeout-sweep exemption.
    *
    * Set by subsystems that retain a long-lived reference to this
    * session_t outside session_manager (e.g., the messaging engine's
    * (provider, address) → session_t map, which sits idle between
    * SMS / chat-app messages for arbitrary durations).
    *
    * `session_cleanup_expired` skips sessions with this flag, even if
    * their idle time exceeds session_timeout_sec.  Without this,
    * `session_destroy` would run, free the session_t after its 3-sec
    * ref-count wait expires, and leave the external subsystem with a
    * dangling pointer.
    *
    * Owning subsystems are responsible for their own lifetime management
    * (the messaging engine evicts via its own LRU and calls
    * session_destroy directly). */
   bool idle_timeout_exempt;
   atomic_uint
       request_generation;  // Incremented on each new request, used to detect superseded requests

   // LLM streaming state (atomic for cross-thread visibility on ARM64)
   atomic_bool llm_streaming_active;  // True while streaming LLM response
   atomic_bool stream_had_content;    // True if any deltas were sent (for fallback)
   atomic_uint current_stream_id;     // Monotonic ID to detect stale deltas
   atomic_bool thinking_active;       // True while streaming thinking/reasoning content

   // Streaming metrics for UI visualization
   uint64_t stream_start_ms;     // Timestamp when LLM call started
   uint64_t first_token_ms;      // Timestamp of first token (0 if none yet)
   uint64_t last_token_ms;       // Timestamp of most recent token
   uint32_t stream_token_count;  // Token count for current stream
   char stream_last_char;        // Last character sent (for sentence spacing fix)

   // Per-session metrics (saved to database after each query)
   session_metrics_tracker_t metrics;
   pthread_mutex_t metrics_mutex;  // Protects metrics (lock level 4)

   // Command tag filter state (strips <command>...</command> from stream)
   // Used when native tool calling is disabled (legacy command tag mode)
   cmd_tag_filter_state_t cmd_tag_filter;  // State for text_filter_command_tags()
   bool cmd_tag_filter_bypass;             // Cached: true if native tools enabled (skip filtering)

   // Active tool tracking (for parallel tool status display)
   char active_tools[8][32];     // Tool names currently executing (max 8 parallel)
   int active_tool_count;        // Number of tools currently executing
   pthread_mutex_t tools_mutex;  // Protects active_tools (lock level 4)

   // Pending visual content (render_visual tool results awaiting assistant message save)
   char *pending_visual;  // Heap-allocated <dawn-visual> content, or NULL

   // Visual guideline cache — tracks which modules have been loaded in this conversation
   // Format: ",diagram,chart," (delimiter-bounded for exact substring matching)
   char visual_modules_loaded[512];

   // Async context compaction (LCM Phase 2 — background compaction between turns)
   async_compaction_t async_compact;

   // Reference counting for safe access (two-phase destruction pattern)
   int ref_count;
   pthread_mutex_t ref_mutex;
   pthread_cond_t ref_zero_cond;  // Signaled when ref_count reaches 0

   // Per-session LLM configuration (allows different LLM for each client)
   session_llm_config_t llm_config;
   pthread_mutex_t llm_config_mutex;  // Protects llm_config (lock level 4)

   // Coding harness: per-session active code project (set via the
   // code_project_set_active tool or the WebUI). The bridge auto-fills cbm
   // tool calls with this project. Protected by llm_config_mutex (a per-session
   // LLM-scoped override, like the other fields under that lock). 0 = none.
   int64_t active_project_id;
   char active_project_name[64];

   // Phase 1f: per-turn focus-injection dedup state.  Shares history_mutex —
   // dedup state is conceptually attached to conversation history (lives or
   // dies with it) and the never-hold-two-L4-locks rule keeps us out of a
   // dedicated lock here.  See injected_set_t above for the full contract.
   injected_set_t injected_set;

   // Phase 1g-i: most-recently-stamped user-message DB id.  Set by
   // session_stamp_last_message_id when role == "user"; read as `turn_id`
   // by dawn_build_prompt to wire the WebSocket context_injection event
   // to the user turn that triggered this prompt rebuild.  Protected by
   // history_mutex (set under it from session_stamp_last_message_id;
   // read via session_get_last_user_msg_id which acquires the lock).
   int64_t last_user_msg_id;

   /* Prompt-cache split drift detection.  FNV-1a hash of the stable
    * prefix from the last per-turn refresh; recomputed each turn and
    * compared.  When it differs the Anthropic cache is invalidated;
    * we log OLOG_INFO once per session so the operator notices silent
    * cache regressions (a settings edit or a future code change that
    * accidentally leaks volatile content into the stable builder).
    * Protected by history_mutex (read+written under it from
    * session_update_system_messages).  See the prompt-cache design doc. */
   uint32_t stable_prefix_hash;
   bool stable_prefix_drift_logged;

   /* Tool-turn persist hook (E2).  Set by the WebUI around a turn so the LLM tool
    * loop can durably persist structured tool messages (assistant tool_calls +
    * role:tool results) via conv_db, keeping the LLM layer decoupled from the DB.
    * Used synchronously on the worker thread that owns the turn; cleared after.
    * NULL when the turn shouldn't persist tool structure (no conversation, etc.). */
   session_tool_persist_fn tool_persist_cb;
   void *tool_persist_userdata;

   /* Tool-loop iteration-boundary hook (live-transcript reorder).  Set by the WebUI
    * around a turn so the streaming bubble can be closed at each tool-calling iteration
    * boundary, keeping the live view's call/result order aligned with the reloaded view.
    * Opt-in (NULL for satellite / local-mic turns); used on the worker thread; cleared
    * after the turn. */
   session_tool_iteration_fn tool_iteration_cb;
   void *tool_iteration_userdata;
} session_t;

// =============================================================================
// Prompt Builder Callback Types (defined before #ifdef so stubs can use them)
// =============================================================================

/**
 * Local-mic prompt builder (SESSION_TYPE_LOCAL).  Returns a static const
 * string owned by the dawn.c command_processing_mode helpers; the
 * session manager never frees the result.  Untouched by Phase 1e —
 * local-mic focus injection lands in a follow-up phase.
 */
typedef const char *(*session_local_prompt_builder_t)(void);

/* ----- Phase 1e: structured per-user prompt builder ---------------------- */

/**
 * Refresh kind hint passed to the per-user prompt builder.
 *
 * In Phase 1e BOTH kinds rebuild every block — the parameter exists as
 * forward-compat for Phase 1f's dedup state, which will let PER_TURN
 * refreshes skip the base+memory rebuild and only recompute the focus
 * block.
 */
typedef enum {
   PROMPT_REFRESH_SESSION_START, /* full rebuild — settings/tools/memory/persona/all blocks */
   PROMPT_REFRESH_PER_TURN,      /* per-turn — IN 1e BOTH KINDS REBUILD ALL BLOCKS */
} prompt_refresh_kind_t;

/**
 * Composed prompt — three named blocks the builder fills in.  The
 * session manager owns the heap allocations on a successful builder
 * return and releases them via `composed_prompt_free()`.  Struct
 * intentionally contains only `char *` so no webui/, tools/, or
 * memory/* type leaks through Layer 1.
 *
 * `focus_block` may be NULL on every refresh kind: when feature is
 * disabled, when focus_compose returns zero candidates, when the focus
 * builder fails.  The composer omits the focus section entirely when
 * the block is NULL or empty so pre-1e output is byte-identical with
 * the feature off.
 */
/* Two typed segments, matching the Claude system-array shape pushed to
 * conversation history per turn:
 *
 *   [0] stable_prefix   ← cache_control: ephemeral (STABLE across turns)
 *                          persona + rules + tool availability
 *                          + User Context + User Identity
 *                          + memory instructions footer
 *   [1] volatile_block  ← per-turn (NO cache_control)
 *                          memory body (preferences/summaries) +
 *                          focus block ([system_time] + retrievals)
 *
 * Anthropic charges 90% less for cache-hit input tokens.  The stable
 * prefix is byte-identical across turns of a session (changes only on
 * settings edits, which we log via the drift counter in session_t),
 * so the cache attaches to it.  The volatile block changes every turn
 * and is never cache-eligible.
 *
 * Claude's formatter (`llm_claude_format.c`) and OpenAI's Responses-API
 * `extract_system_instructions` both iterate over multiple consecutive
 * `role: "system"` messages and handle the two-segment shape correctly.
 * The legacy `prompt_compose_to_string()` flattens them with `\n\n`
 * separator for callers that want a single string (settings refresh,
 * WebUI system-prompt inspector).
 *
 * SIZE INVARIANT (see _Static_assert below): adding a field requires
 * updating both `composed_prompt_free` impls (prompt_compose.c
 * out-of-line + the static-inline fallback in this header below for
 * !ENABLE_MULTI_CLIENT).  The sizeof assert forces a build break at
 * the new field add site so the parallel-impl drift surfaces loudly. */
typedef struct {
   char *stable_prefix;  /* Cacheable segment — persona/rules/identity/
                            footer.  Byte-identical across turns absent
                            settings change.  Anthropic cache_control
                            attaches here. */
   char *volatile_block; /* Per-turn segment — memory body + focus block.
                            Rebuilt every turn; never cache-eligible. */
} composed_prompt_t;

/* Pin field count so a future add forces both composed_prompt_free
 * implementations to update in lockstep.  Two char* fields → 16 bytes
 * on LP64.  If you bump this, also update the static-inline
 * composed_prompt_free below AND prompt_compose_free in
 * src/core/prompt_compose.c. */
_Static_assert(sizeof(composed_prompt_t) == 2 * sizeof(char *),
               "composed_prompt_t field add detected — update BOTH "
               "composed_prompt_free (this header) AND prompt_compose_free "
               "(src/core/prompt_compose.c) to release the new field, then "
               "bump this assertion.");

/**
 * Per-user prompt builder — replaces the legacy
 * `session_user_prompt_builder_t (int)` callback.  Implementation
 * lives in src/webui/ (Layer 4); session_manager (Layer 1) only knows
 * the function pointer.
 *
 * Contract:
 *   SUCCESS — `*out` is populated; session_manager owns the heap
 *             allocations and releases them via composed_prompt_free.
 *             A NULL `focus_block` is valid — it just omits the
 *             focus section in the composed string.
 *   FAILURE — Builder MUST clean up any partial allocation.  Caller
 *             treats this as a refresh failure; no system-prompt swap
 *             occurs.
 */
typedef int (*session_prompt_builder_t)(int user_id,
                                        const char *user_turn_text,
                                        prompt_refresh_kind_t kind,
                                        composed_prompt_t *out);

/* =============================================================================
 * Phase 1f: dedup state APIs
 *
 * Asymmetric naming — every function with `_locked` suffix REQUIRES the
 * caller to hold `session->history_mutex`; the unsuffixed `_clear` is
 * self-locking and must NOT be called while history_mutex is held.  The
 * naming makes the contract loud (per architecture review on
 * asymmetric-mutex footgun pattern).
 * ============================================================================= */

#ifdef ENABLE_MULTI_CLIENT
/**
 * @brief Look up a (source_id, item_id) entry in the session's dedup set.
 *
 * Caller MUST hold `session->history_mutex`.  Returns SUCCESS with `*out`
 * populated on hit; FAILURE with `*out` untouched on miss.  NULL inputs
 * return FAILURE without dereferencing.
 *
 * @param session Session whose dedup set to query
 * @param source_id Adapter source id (NUL-terminated, ≤31 chars)
 * @param item_id Opaque item id (NUL-terminated, ≤63 chars)
 * @param[out] out Populated on hit; untouched on miss
 * @return SUCCESS on hit, FAILURE on miss / NULL input
 */
int session_injected_set_lookup_locked(const session_t *session,
                                       const char *source_id,
                                       const char *item_id,
                                       injected_set_entry_t *out);

/**
 * @brief Insert-or-update a (source_id, item_id) entry in the dedup set.
 *
 * Caller MUST hold `session->history_mutex`.  Updates `last_injected_turn`
 * to the session's current turn counter and `last_score` to the supplied
 * value.  If the entry is new and the set is at capacity, the oldest
 * entry by `last_injected_turn` is evicted (LRU).
 *
 * Logs OLOG_INFO once-per-session on the first eviction so long-
 * conversation behavior is visible without spamming the log.
 *
 * @return Always SUCCESS unless the inputs are malformed (NULL session /
 *         source_id / item_id), in which case FAILURE.
 */
int session_injected_set_record_locked(session_t *session,
                                       const char *source_id,
                                       const char *item_id,
                                       float score);

/**
 * @brief Pre-increment-and-return the session's monotonic turn counter.
 *
 * Caller MUST hold `session->history_mutex`.  Returns the new counter
 * value (post-increment).  Wraps at INT_MAX (cosmetic — would take ~68
 * billion turns at one per second; embedded boxes never reach this).
 */
int session_injected_set_advance_turn_locked(session_t *session);

/**
 * @brief Reset the session's dedup state and per-session log gates.
 *
 * SELF-LOCKING — acquires `session->history_mutex` internally.  MUST
 * NOT be called while history_mutex is already held.
 *
 * Use on PROMPT_REFRESH_SESSION_START (capability/setting refresh, fresh
 * session start) so subsequent PER_TURN refreshes admit all candidates
 * fresh and once-per-session log lines re-arm.
 */
void session_injected_set_clear(session_t *session);

/**
 * @brief Set/get the thread-local session pointer used by the per-turn
 *        focus-block builder to find the dedup set.
 *
 * `session_dispatch_user_turn` sets this to its session before invoking
 * the registered prompt builder and clears it after.  build_focus_block
 * reads it via the getter to avoid plumbing session_t through the
 * builder typedef.  NULL on any non-PER_TURN path (SESSION_START,
 * standalone build_system_prompt_string callers) — build_focus_block
 * skips dedup when getter returns NULL.
 *
 * Thread-local storage: each worker thread owns its own dispatch
 * pointer, so concurrent sessions on different threads do not collide.
 */
void session_set_dispatch_session(session_t *session);
session_t *session_get_dispatch_session(void);
#endif /* ENABLE_MULTI_CLIENT */

// =============================================================================
// Lifecycle Functions
// =============================================================================

#ifdef ENABLE_MULTI_CLIENT
/**
 * @brief Initialize the session manager
 *
 * Creates the rwlock and initializes the local session (session_id = 0).
 * Must be called before any other session_* functions.
 *
 * @return 0 on success, 1 on failure
 */
int session_manager_init(void);

/**
 * @brief Cleanup the session manager
 *
 * Destroys all active sessions and frees resources.
 * Should be called during application shutdown.
 */
void session_manager_cleanup(void);
#endif /* ENABLE_MULTI_CLIENT */

// =============================================================================
// Session Creation and Retrieval
// =============================================================================

#ifdef ENABLE_MULTI_CLIENT
/**
 * @brief Create new session
 *
 * @param type Session type (LOCAL, DAP, DAP2, WEBSOCKET)
 * @param client_fd Client socket (-1 for local session)
 * @return New session pointer, or NULL if max sessions reached
 *
 * @locks session_manager_rwlock (write)
 * @lock_order 1
 */
session_t *session_create(session_type_t type, int client_fd);

/**
 * @brief Create new DAP2 session with identity (from REGISTER message)
 *
 * @param client_fd Client socket
 * @param tier DAP2_TIER_1 (text) or DAP2_TIER_2 (audio)
 * @param identity Satellite identity (UUID, name, location)
 * @param capabilities Satellite capabilities (local ASR/TTS/wake word)
 * @return New session, or existing session if UUID+secret matches (reconnection)
 *
 * @locks session_manager_rwlock (write) for entire operation (atomic check-and-create)
 * @note For NEW sessions: generates reconnect_secret in session->identity
 * @note For RECONNECTION (UUID matches existing session):
 *       - If identity->reconnect_secret matches existing: reclaims session
 *       - If secret is empty or doesn't match: creates NEW session (security)
 *       - On reclaim: updates client_fd, clears disconnected, returns existing
 */
session_t *session_create_dap2(int client_fd,
                               dap2_tier_t tier,
                               const dap2_identity_t *identity,
                               const dap2_capabilities_t *capabilities);

/**
 * @brief Get the reconnect secret for a session
 *
 * Returns the secret that must be included in reconnection requests.
 * Caller must free the returned string.
 *
 * @param session Session to query
 * @return Allocated secret string (caller frees), or NULL if not a DAP2 session
 */
char *session_get_reconnect_secret(session_t *session);

/**
 * @brief Get or create DAP1 session by client IP (legacy protocol)
 *
 * DAP1 clients don't send a REGISTER message with UUID, so we use IP address
 * as a simple session identifier for testing purposes. This allows conversation
 * history to persist across reconnections from the same IP.
 *
 * @param client_fd Client socket
 * @param client_ip Client IP address string
 * @return Existing session if IP matches, or new session if not found
 *
 * @locks session_manager_rwlock (write) for entire operation
 * @note If IP matches an existing DAP session:
 *       1. Updates client_fd
 *       2. Clears disconnected flag
 *       3. Increments ref_count
 *       4. Returns existing session with preserved conversation history
 * @note Caller MUST call session_release() when done
 * @warning This is a temporary solution for DAP1 testing. DAP2 uses proper UUIDs.
 */
session_t *session_get_or_create_dap(int client_fd, const char *client_ip);

/**
 * @brief Get session by ID (increments ref_count while holding rwlock)
 *
 * @param session_id Session ID to look up
 * @return Session pointer (caller must call session_release), or NULL if not found
 *
 * @locks session_manager_rwlock (read), then session->ref_mutex (brief)
 * @lock_order Acquires rwlock, increments ref, releases rwlock BEFORE caller uses session
 * @note Caller MUST call session_release() when done
 * @note Returns NULL for disconnected sessions (prevents new refs to dying sessions)
 */
session_t *session_get(uint32_t session_id);

/**
 * @brief Get session by ID for reconnection (allows disconnected sessions)
 *
 * Similar to session_get() but returns disconnected sessions to allow
 * WebSocket clients to reconnect with their existing conversation history.
 *
 * @param session_id Session ID to look up
 * @return Session pointer (caller must call session_release), or NULL if not found
 *
 * @note Caller should clear the disconnected flag after reconnecting
 */
session_t *session_get_for_reconnect(uint32_t session_id);

/**
 * @brief Find session by DAP2 UUID and retain it
 *
 * Searches for a DAP2 session matching the given UUID. If found,
 * increments ref_count before returning (caller must session_release).
 *
 * @param uuid DAP2 satellite UUID string
 * @return Session pointer (caller must release), or NULL if not found
 *
 * @locks session_manager_rwlock (read), then session->ref_mutex (brief)
 * @note Returns NULL for disconnected sessions
 */
session_t *session_find_by_uuid(const char *uuid);

/**
 * @brief Retain session reference (increments ref_count)
 *
 * Use this when passing a session pointer to another thread or storing
 * it for later use. Prevents session destruction while reference is held.
 *
 * @param session Session to retain
 * @note Caller MUST call session_release() when done
 */
void session_retain(session_t *session);

/**
 * @brief Release session reference (decrements ref_count)
 *
 * @param session Session to release
 * @note Signals ref_zero_cond when ref_count reaches 0
 */
void session_release(session_t *session);

/**
 * @brief Get local session (always exists)
 *
 * @return Local session pointer (session_id = 0)
 * @note Does NOT increment ref_count (local session is never destroyed)
 */
session_t *session_get_local(void);
#endif /* ENABLE_MULTI_CLIENT */

// =============================================================================
// Session Destruction
// =============================================================================

#ifdef ENABLE_MULTI_CLIENT

/**
 * @brief Mark session as disconnected and destroy when ref_count=0
 *
 * @param session_id Session ID to destroy
 *
 * @note Two-phase destruction:
 *       1. Mark disconnected + remove from active list (prevents new refs)
 *       2. Wait for ref_count=0 via ref_zero_cond, then free
 */
void session_destroy(uint32_t session_id);

/**
 * @brief Cleanup expired sessions (called periodically)
 *
 * Iterates through all sessions and destroys those that have exceeded
 * SESSION_TIMEOUT_SEC without activity.
 */
void session_cleanup_expired(void);

/**
 * @brief Check all sessions for idle conversation timeout
 *
 * Iterates non-local sessions and saves+clears conversation history
 * for any session that has been idle longer than conversation_idle_timeout_min.
 * Called periodically from auth_maintenance thread.
 */
void session_check_idle_conversations(void);
#endif /* ENABLE_MULTI_CLIENT */

// =============================================================================
// Conversation History
// =============================================================================

#ifdef ENABLE_MULTI_CLIENT

/**
 * @brief Add message to session's conversation history
 *
 * @param session Session to update
 * @param role Message role ("user", "assistant", "system")
 * @param content Message content
 *
 * @locks session->history_mutex
 * @lock_order 3
 */
void session_add_message(session_t *session, const char *role, const char *content);

/**
 * @brief Inject a "system" message into every live interactive session.
 *
 * Interactive = local mic (SESSION_TYPE_LOCAL), satellites (DAP/DAP2), and
 * WebUI — the surfaces from which a user can issue a real-time command.
 * Messaging-channel sessions (SESSION_TYPE_MESSAGING) are intentionally
 * excluded: injecting a transient device event into an unrelated ongoing chat
 * thread is noise.
 *
 * Used to fan out device-wide events that any interactive surface should be
 * able to act on (e.g. an incoming phone call ringing, or a call that was just
 * answered/ended elsewhere) instead of only the local session seeing them.
 *
 * Follows the standard snapshot-then-retain iteration: session IDs are
 * snapshotted under the module read lock, then each session is retained via
 * session_get() and written under its own history_mutex, so no per-session
 * lock is ever held while the module lock is held.
 *
 * @param content Message content (NULL/empty is a no-op).
 * @return Number of sessions the message was written into.
 *
 * @locks session_manager_rwlock (read) for snapshot, then per-session
 *        ref_mutex + history_mutex during apply.
 */
int session_broadcast_system_message(const char *content);

/**
 * @brief Like session_broadcast_system_message() but skips the LOCAL session.
 *
 * The local session (sessions[0], voice pipeline) is appended-to by the main
 * thread WITHOUT history_mutex, so a foreign thread must never write it directly
 * (history_mutex won't serialize against the unlocked main-path append). Callers
 * running off the main thread (e.g. the phone broadcaster on the MQTT/echo
 * thread) use this to fan an event out to the WebUI/DAP/DAP2 surfaces — which
 * ARE fully serialized by their own history_mutex — and defer the local write to
 * the main loop via pending_sysmsg_push(). On-main-thread callers should keep
 * using session_broadcast_system_message(), which includes the local session.
 *
 * @param content Message content (NULL/empty is a no-op).
 * @return Number of (non-local) sessions the message was written into.
 */
int session_broadcast_system_message_nonlocal(const char *content);

/**
 * @brief Stamp a DB row ID onto the most recent unstamped history entry for role.
 *
 * Scans backward through conversation_history for the last entry with matching
 * role that has no "id" field, then sets "id" = msg_id. Used after
 * conv_db_add_message_ex() to link in-memory history entries to their DB row IDs
 * so the extraction filter can use ID-based cursors.
 */
void session_stamp_last_message_id(session_t *session, const char *role, int64_t msg_id);

/**
 * @brief Get the DB id of the session's most recently stamped USER message.
 *
 * Phase 1g-i: returns `session->last_user_msg_id` under `history_mutex`.
 * Returns 0 when no user message has been stamped on this session yet
 * (fresh session, or pre-stamping caller path).  WebUI prompt builder
 * uses this as the `turn_id` field of the `context_injection` WebSocket
 * event — every user message that hits `conv_db_add_message_ex` and
 * gets stamped is the "turn" that triggers the next prompt rebuild.
 *
 * @note Thread-safe — acquires `session->history_mutex`.
 * @return msg_id (>0) or 0 if not set.
 */
int64_t session_get_last_user_msg_id(session_t *session);

/**
 * @brief Add message with images to session's conversation history
 *
 * Creates a multi-part content message in OpenAI format:
 * { "role": "user", "content": [
 *     { "type": "text", "text": "..." },
 *     { "type": "image_url", "image_url": { "url": "data:image/jpeg;base64,..." } },
 *     ...
 * ]}
 *
 * This allows images to persist across conversation turns for follow-up questions.
 *
 * @param session Session to update
 * @param role Message role ("user" typically)
 * @param text Text content of the message
 * @param vision_images Array of base64-encoded image strings
 * @param vision_image_count Number of images in array
 *
 * @locks session->history_mutex
 * @lock_order 3
 */
void session_add_message_with_images(session_t *session,
                                     const char *role,
                                     const char *text,
                                     const char *const *vision_images,
                                     int vision_image_count);

/**
 * @brief Append a pre-built message object to conversation history.
 *
 * Takes ownership of @p message (a complete `{role, content, ...}` json_object,
 * e.g. a multi-part vision message or a reconstructed tool message) and appends
 * it under history_mutex.  On any error (NULL args, OOM) @p message is freed.
 * Used by conversation-reload rehydration where the caller builds the exact
 * LLM-faithful shape (real per-image mime, tool_calls fields) itself.
 *
 * @param session Target session
 * @param message Owned json_object to append (consumed in all paths)
 *
 * @locks session->history_mutex
 * @lock_order 3
 */
void session_add_message_multipart(session_t *session, struct json_object *message);

/**
 * @brief Set (or clear) the tool-turn persist hook for a session.
 *
 * The WebUI sets this around a turn so the LLM tool loop can persist structured
 * tool messages. @p cb and @p userdata are read synchronously on the worker thread
 * that owns the turn; pass NULL/NULL to clear after the turn completes. @p userdata
 * lifetime must cover the turn (the caller owns it).
 */
void session_set_tool_persist_hook(session_t *session, session_tool_persist_fn cb, void *userdata);

/**
 * @brief Set (or clear) the tool-loop iteration-boundary hook for a session.
 *
 * The WebUI sets this around a turn so the live streaming bubble is closed at each
 * tool-calling iteration boundary (so call/result entries interleave with text the
 * way a reloaded conversation does). @p cb / @p userdata are read on the worker thread
 * that owns the turn; pass NULL/NULL to clear after the turn. Opt-in: satellite and
 * local-mic turns never set it and are unaffected.
 */
void session_set_tool_iteration_hook(session_t *session,
                                     session_tool_iteration_fn cb,
                                     void *userdata);

/**
 * @brief Get conversation history JSON (for LLM API calls)
 *
 * @param session Session to query
 * @return JSON array of messages (caller must json_object_put when done)
 *
 * @locks session->history_mutex
 * @note Returns a reference to the internal array, not a copy
 */
struct json_object *session_get_history(session_t *session);

/**
 * @brief Clear conversation history
 *
 * @param session Session to clear
 *
 * @locks session->history_mutex
 */
void session_clear_history(session_t *session);

/**
 * @brief Check if session has user messages (beyond system prompt)
 *
 * @param session Session to check
 * @return true if session has at least 2 messages (system + user), false otherwise
 *
 * @locks session->history_mutex
 */
bool session_has_messages(session_t *session);

/**
 * @brief Update last interaction complete timestamp
 *
 * Called after a successful wake-word-to-response cycle completes.
 * Used for idle timeout detection in voice conversations.
 *
 * @param session Session to update
 */
void session_update_interaction_complete(session_t *session);

/**
 * @brief Save voice conversation to database and start fresh
 *
 * Called on idle timeout for Session 0 (local mic) and DAP clients.
 * Creates a new conversation in the database with origin='voice',
 * saves all messages, triggers memory extraction, then clears session history.
 *
 * @param session Session to save
 * @param conv_id_out Output: conversation ID on success (must not be NULL)
 * @return 0 on success, 1 on failure
 *
 * @locks session->history_mutex
 */
int session_save_voice_conversation(session_t *session, int64_t *conv_id_out);

/**
 * @brief Initialize session with system prompt
 *
 * Clears any existing history and adds the system message.
 * Used for session initialization and conversation reset.
 *
 * @param session Session to initialize
 * @param system_prompt System prompt content (AI description/personality)
 *
 * @locks session->history_mutex
 */
void session_init_system_prompt(session_t *session, const char *system_prompt);

/**
 * @brief Append room context to session's system prompt
 *
 * Appends "\nRoom=<room>." to the existing system prompt so the LLM knows
 * which room the voice command originates from. No-op if room is NULL or empty.
 *
 * @param session Session to update
 * @param room Room name (e.g., "kitchen", "office")
 *
 * @locks session->history_mutex (via session_get/update_system_prompt)
 */
void session_append_room_context(session_t *session, const char *room);

/**
 * @brief Append satellite room and HA area context to system prompt
 *
 * Builds both Room and HomeAssistant_Area in a single stack buffer.
 * Sanitizes ha_area for non-printable characters.
 *
 * @param session Session to update
 * @param room Room name from satellite identity
 * @param ha_area Home Assistant area (can be NULL or empty)
 *
 * @locks session->history_mutex (via session_get/update_system_prompt)
 */
void session_append_satellite_context(session_t *session, const char *room, const char *ha_area);

/**
 * @brief Update the system prompt without clearing conversation history
 *
 * Finds the existing system message in the conversation history and updates
 * its content to the new prompt. If no system message exists, creates one
 * at the beginning. Unlike session_init_system_prompt(), this preserves
 * the existing conversation.
 *
 * @param session Session to update
 * @param system_prompt New system prompt content
 *
 * @locks session->history_mutex
 */
void session_update_system_prompt(session_t *session, const char *system_prompt);

/**
 * @brief Update the session's system messages as two segments.
 *
 * Rebuilds the conversation history so message[0] is a `role: "system"`
 * carrying @p stable_prefix and message[1] is a `role: "system"`
 * carrying @p volatile_block, followed by all existing non-system
 * messages in original order.  The two-segment shape is what lets
 * Anthropic attach `cache_control: ephemeral` to the (byte-identical
 * across turns) stable prefix and skip the per-turn volatile block.
 *
 * Either segment may be NULL or empty — that segment is omitted (no
 * empty system message is pushed).  Refusing to push BOTH-NULL means
 * the system prompt is unchanged; caller is responsible for passing
 * at least one segment.
 *
 * Also computes the FNV-1a hash of @p stable_prefix and compares to
 * the prior turn's hash on @p session.  If they differ AND drift has
 * not yet been logged this session, emits OLOG_INFO once.  The first
 * call on a fresh session always "drifts" (prior hash is 0); the
 * helper suppresses that boot-time log line.
 *
 * @param session         Session to update.
 * @param stable_prefix   Cacheable segment (may be NULL/empty).
 * @param volatile_block  Per-turn segment (may be NULL/empty).
 *
 * @locks session->history_mutex
 */
void session_update_system_messages(session_t *session,
                                    const char *stable_prefix,
                                    const char *volatile_block);

/**
 * @brief Append a per-turn hint string to the LAST system message.
 *
 * Used by callers that want to attach a one-turn instruction (e.g.
 * the messaging engine's SMS channel hint about outbound truncation)
 * to the volatile segment rather than the stable cached prefix.
 * Appends `\n\n<hint>` to the content of the last system message
 * found in the conversation history.
 *
 * In the two-segment shape (post `session_update_system_messages`)
 * the last system message is the volatile block; legacy single-
 * message sessions just see the hint appended to their lone system
 * message.  Either way the cacheable stable prefix is untouched.
 *
 * No-op on NULL session, NULL/empty text, or when no system message
 * exists.
 *
 * @param session Session to update.
 * @param text    Hint text (NUL-terminated, may be NULL/empty).
 *
 * @locks session->history_mutex
 */
void session_append_to_volatile_segment(session_t *session, const char *text);

/**
 * @brief Get the system prompt from a session
 *
 * Returns the content of the first "system" role message in the conversation
 * history. Useful for debugging to see what instructions the LLM received.
 *
 * @param session Session to query
 * @return System prompt string, or NULL if not found. Caller must free().
 *
 * @locks session->history_mutex
 */
char *session_get_system_prompt(session_t *session);

/**
 * @brief Concatenate every role:"system" message into a single string.
 *
 * The two-segment shape introduced by `session_update_system_messages`
 * stores stable + volatile as separate system messages so Anthropic
 * cache_control can attach to the stable one only.  `session_get_
 * system_prompt` returns the FIRST system message (stable only) to
 * preserve the legacy single-message semantics used by
 * `session_append_room_context` / `session_append_satellite_context`
 * — they read-modify-write the stable segment.
 *
 * This helper exists for callers that want the FULL view (debug
 * inspectors, system-prompt UI panels).  Returns stable + "\n\n" +
 * volatile in two-segment sessions, or just the lone system message
 * in legacy single-message sessions.  NULL when no system message
 * exists.  Caller frees.
 *
 * @locks session->history_mutex
 */
char *session_get_full_system_prompt(session_t *session);

/**
 * @brief Get the content of the most recent message with the given role.
 *
 * Walks `session->conversation_history` backwards and returns the
 * content string of the first message whose `role` matches.  Returns
 * NULL if no match, or if the matched message stores content as an
 * array (multi-part vision messages) rather than a plain string.
 *
 * @param session Session to query.
 * @param role    Role to match ("user" / "assistant" / "system").
 * @return Allocated string (caller frees), or NULL.
 *
 * @locks session->history_mutex
 */
char *session_get_last_message_content(session_t *session, const char *role);

/**
 * @brief Replace the content of the most recent message with the given role.
 *
 * Walks `session->conversation_history` backwards, finds the first
 * message whose `role` matches, and replaces its `content` field with
 * `new_content`.  Used by the messaging engine to align session
 * history with what was actually delivered when outbound truncation
 * happens — the LLM then sees the truncated text rather than the
 * full one when composing the next turn.
 *
 * No-op on multi-part (array) content; only operates on plain-string
 * content.
 *
 * @param session     Session to mutate.
 * @param role        Role to match.
 * @param new_content Replacement content (copied).
 * @return true if a message was found and replaced, false otherwise.
 *
 * @locks session->history_mutex
 */
bool session_replace_last_message_content(session_t *session,
                                          const char *role,
                                          const char *new_content);

/**
 * @brief Register the structured per-user prompt builder used by the
 *        refresh helper (Phase 1e).
 *
 * Called once during dawn.c init to wire the session manager to the
 * WebUI's structured prompt builder (`dawn_build_prompt`) without
 * creating a direct dependency from core into webui.  Replaces the
 * legacy single-string `session_user_prompt_builder_t` registration
 * (removed in 1e).
 *
 * May be called with NULL to clear.
 *
 * @param fn Builder function pointer (may be NULL)
 */
void session_manager_set_prompt_builder(session_prompt_builder_t fn);

/**
 * @brief Register the local (mic) prompt builder used by the refresh helper
 *
 * Called once during dawn.c init. If unset, refresh falls back to
 * get_local_command_prompt() for local sessions.
 *
 * @param fn Builder function pointer (may be NULL)
 */
void session_manager_set_local_prompt_builder(session_local_prompt_builder_t fn);

/**
 * @brief Free the heap allocations inside a composed_prompt_t.
 *
 * Idempotent on a zeroed / already-freed struct.  Resets all three
 * pointers to NULL so the struct is safe to reuse.
 */
void composed_prompt_free(composed_prompt_t *p);

/**
 * @brief Flatten a composed_prompt_t into a single allocated string.
 *
 * Concatenates `base_prompt + memory_block + focus_block` with the
 * existing `--- USER MEMORY ---` framing identity for the memory
 * section and a parallel `--- TURN CONTEXT ---` framing for the
 * focus section.  NULL or empty `focus_block` omits the focus
 * section entirely (preserves byte-identical pre-1e output when
 * feature is disabled).
 *
 * @return Caller-owned string; NULL on OOM or NULL input.
 */
char *session_manager_compose_prompt_string(const composed_prompt_t *blocks);

/**
 * @brief Build the full per-user system prompt as a flat string,
 *        matching the legacy `build_user_prompt` ownership semantics.
 *
 * Convenience wrapper used by capability-change refresh paths that
 * need a `char *` to hand to `session_update_system_prompt`.  Calls
 * the registered structured builder with `kind=SESSION_START` and
 * `user_turn_text=NULL`, then flattens the result via
 * `session_manager_compose_prompt_string`, which transparently picks
 * up any future `composed_prompt_t` block — callers do NOT need to
 * update this function when 1f / 1g add blocks.
 *
 * If you need block-level access (e.g., for caching individual
 * blocks across PER_TURN refreshes, or for surfacing per-block
 * provenance in a UI), call the structured builder directly via
 * the `session_prompt_builder_t` registered with
 * `session_manager_set_prompt_builder` instead.
 *
 * @return Caller-owned string; NULL on builder failure.
 */
char *session_manager_build_system_prompt_string(int user_id);

/**
 * @brief Per-turn refresh helper — called from every user-message-to-LLM
 *        dispatch site BEFORE the LLM dispatch.
 *
 * Calls the registered prompt builder with `kind=PER_TURN` and the
 * user's turn text, flattens the result, and swaps the session's
 * system message in place via `session_update_system_prompt`.
 *
 * Always returns SUCCESS even when the focus refresh fails — failure
 * is logged and `focus_block` is set to NULL; LLM dispatch is never
 * blocked by focus errors.  The previous turn's focus_block content
 * NEVER survives into the next turn (cross-turn isolation invariant).
 *
 * Synchronous.  All in-1e-scope dispatch sites (webui text worker,
 * webui audio worker, satellite worker) run on dedicated worker
 * threads; no worker_pool dispatch needed.  The lws service thread
 * MUST NOT call this — verified via the per-site audit.
 */
int session_dispatch_user_turn(session_t *session, const char *user_turn_text);

/**
 * @brief Rebuild the system prompt on every active session, preserving history
 *
 * Walks all active sessions and replaces each session's system-role message
 * with a freshly-built prompt. Conversation history (user/assistant/tool
 * messages) is preserved — only the system message content changes.
 *
 * Prompt selection per session type:
 *   - SESSION_TYPE_LOCAL:  get_local_command_prompt()
 *   - Other, with user_id: registered user_prompt_builder (if any)
 *   - Other, no user_id:   get_remote_command_prompt()
 *
 * Call this after invalidate_system_instructions() when capabilities change
 * mid-conversation (e.g., HUD goes online/offline, tool config saved) so the
 * LLM sees the new availability on its next turn.
 *
 * @locks session_manager_rwlock (read) for snapshot, then per-session
 *        history_mutex via session_update_system_prompt
 */
void session_manager_refresh_all_prompts(void);
#endif /* ENABLE_MULTI_CLIENT */

// =============================================================================
// LLM Integration
// =============================================================================

#ifdef ENABLE_MULTI_CLIENT

/**
 * @brief Call LLM with session's conversation history
 *
 * @param session Session with conversation context
 * @param user_text User's query text
 * @return LLM response (caller must free), or NULL on error/cancel
 *
 * @note Adds user message before call, assistant response after call
 * @note Returns NULL if session->disconnected is set (cancel)
 * @note Room context is in the system prompt (not prepended to user text)
 * @note Uses session's own LLM config (copied from defaults at session creation)
 */
char *session_llm_call(session_t *session, const char *user_text);

/**
 * @brief Sentence callback for TTS streaming
 *
 * Called for each complete sentence detected in the LLM response.
 * Use this to generate and send audio sentence-by-sentence.
 *
 * @param sentence Complete sentence text
 * @param userdata User-provided context pointer
 */
typedef void (*session_sentence_callback)(const char *sentence, void *userdata);

/**
 * @brief Call LLM with sentence-by-sentence TTS streaming
 *
 * Like session_llm_call(), but uses sentence buffering to call the provided
 * callback for each complete sentence. This enables real-time audio streaming
 * where TTS is generated and sent per-sentence rather than waiting for the
 * full response.
 *
 * @param session Session with conversation context
 * @param user_text User's query text
 * @param sentence_cb Callback for each complete sentence (for TTS)
 * @param userdata Context passed to sentence callback
 * @return LLM response (caller must free), or NULL on error/cancel
 *
 * @note Does NOT do WebSocket text streaming (use for audio-input sessions)
 * @note Adds user message before call, assistant response after call
 */
char *session_llm_call_with_tts(session_t *session,
                                const char *user_text,
                                session_sentence_callback sentence_cb,
                                void *userdata);

/**
 * @brief Unified LLM call with optional TTS and vision, without adding user message
 *
 * Flexible LLM call that supports:
 * - Optional vision images (pass NULL/0 for text-only)
 * - Optional TTS sentence streaming (pass NULL for no TTS)
 *
 * Use when caller has already added the message before the call.
 * This ensures message is in history even if the call is cancelled.
 *
 * @param session Session context
 * @param user_text User input text
 * @param vision_images Array of base64 encoded image data (NULL for text-only)
 * @param vision_image_sizes Array of image sizes (NULL for text-only)
 * @param vision_mimes Array of MIME type strings (NULL for text-only)
 * @param vision_image_count Number of images (0 for text-only)
 * @param sentence_cb Callback for each complete sentence (NULL to disable TTS)
 * @param userdata Context passed to sentence callback
 * @return LLM response (caller must free), or NULL on failure
 */
char *session_llm_call_with_tts_vision_no_add(session_t *session,
                                              const char *user_text,
                                              const char **vision_images,
                                              const size_t *vision_image_sizes,
                                              const char (*vision_mimes)[24],
                                              int vision_image_count,
                                              session_sentence_callback sentence_cb,
                                              void *userdata);
#endif /* ENABLE_MULTI_CLIENT */

// =============================================================================
// Per-Session LLM Configuration
// =============================================================================

#ifdef ENABLE_MULTI_CLIENT

/**
 * @brief Set per-session LLM configuration
 *
 * Validates that requested provider has API key before applying.
 *
 * @param session Session to configure
 * @param config LLM configuration to apply
 * @return 0 on success, 1 if requested provider lacks API key
 *
 * @locks session->llm_config_mutex
 */
int session_set_llm_config(session_t *session, const session_llm_config_t *config);

/**
 * @brief Get session's current LLM configuration
 *
 * @param session Session to query
 * @param config Output: copy of session's LLM config
 *
 * @locks session->llm_config_mutex
 */
void session_get_llm_config(session_t *session, session_llm_config_t *config);

/**
 * @brief Reset session LLM config to defaults from dawn.toml
 *
 * Resets session to use default settings from configuration file.
 * Changes only affect this session, not others.
 *
 * @param session Session to reset
 *
 * @locks session->llm_config_mutex
 */
void session_clear_llm_config(session_t *session);
#endif /* ENABLE_MULTI_CLIENT */

// =============================================================================
// Per-Session Metrics
// =============================================================================

#ifdef ENABLE_MULTI_CLIENT

/**
 * @brief Record a completed LLM query in session metrics
 *
 * Updates session metrics with query counts, token usage, and performance data.
 * Automatically persists to database using UPSERT pattern (INSERT on first call,
 * UPDATE on subsequent calls).
 *
 * @param session Session to update
 * @param provider LLM provider used ("openai", "claude", "local")
 * @param tokens_in Input tokens consumed
 * @param tokens_out Output tokens generated
 * @param tokens_cached Cached tokens (prompt caching)
 * @param llm_ttft_ms Time to first token in milliseconds
 * @param llm_total_ms Total LLM response time in milliseconds
 * @param is_error Whether the query resulted in an error
 *
 * @locks session->metrics_mutex
 */
void session_record_query(session_t *session,
                          const char *provider,
                          uint64_t tokens_in,
                          uint64_t tokens_out,
                          uint64_t tokens_cached,
                          double llm_ttft_ms,
                          double llm_total_ms,
                          bool is_error);

/**
 * @brief Record ASR timing for session metrics
 *
 * @param session Session to update
 * @param asr_ms ASR processing time in milliseconds
 *
 * @locks session->metrics_mutex
 */
void session_record_asr_timing(session_t *session, double asr_ms);

/**
 * @brief Record TTS timing for session metrics
 *
 * @param session Session to update
 * @param tts_ms TTS processing time in milliseconds
 *
 * @locks session->metrics_mutex
 */
void session_record_tts_timing(session_t *session, double tts_ms);

/**
 * @brief Record full pipeline timing for session metrics
 *
 * @param session Session to update
 * @param pipeline_ms Total pipeline time (ASR + LLM + TTS) in milliseconds
 *
 * @locks session->metrics_mutex
 */
void session_record_pipeline_timing(session_t *session, double pipeline_ms);

/**
 * @brief Set user ID for session metrics
 *
 * Called when WebSocket session is authenticated to associate metrics with user.
 *
 * @param session Session to update
 * @param user_id Authenticated user ID
 *
 * @locks session->metrics_mutex
 */
void session_set_metrics_user(session_t *session, int user_id);

#endif /* ENABLE_MULTI_CLIENT */

// =============================================================================
// Utility Functions
// =============================================================================

#ifdef ENABLE_MULTI_CLIENT

/**
 * @brief Update session's last activity timestamp
 *
 * @param session Session to update
 */
void session_touch(session_t *session);

/**
 * @brief Get session count (for metrics)
 *
 * @return Number of active sessions
 */
int session_count(void);

/**
 * @brief Get session type name as string
 *
 * @param type Session type
 * @return Human-readable type name
 */
const char *session_type_name(session_type_t type);

#endif /* ENABLE_MULTI_CLIENT */

// =============================================================================
// Command Context (Thread-Local)
// =============================================================================

#ifdef ENABLE_MULTI_CLIENT

/**
 * @brief Set the current command context session for this thread
 *
 * Call this before processing commands to establish which session's
 * LLM config should be used by device callbacks (e.g., LLM switch commands).
 * The context is thread-local and should be cleared after command processing.
 *
 * @param session Session to use for command context (NULL to clear)
 */
void session_set_command_context(session_t *session);

/**
 * @brief Get the current command context session for this thread
 *
 * Used by device callbacks (e.g., switch_llm_tool) to get the session
 * whose LLM config should be modified by the command.
 *
 * @return Current command context session, or NULL if not set
 */
session_t *session_get_command_context(void);
#endif /* ENABLE_MULTI_CLIENT */

/* =============================================================================
 * Command Context Scope Guard (GCC/Clang cleanup attribute)
 *
 * Provides automatic cleanup of command context when a scope exits, even on
 * early returns. Uses __attribute__((cleanup)) for RAII-style safety in C.
 *
 * Usage:
 *   {
 *      SESSION_SCOPED_COMMAND_CONTEXT(my_session);
 *      // ... do work with command context set ...
 *      // Context automatically cleared when scope exits
 *   }
 * ============================================================================= */

#ifdef ENABLE_MULTI_CLIENT
/**
 * @brief Cleanup function for scope guard - clears command context
 * @param ctx Pointer to session pointer (unused, just for cleanup signature)
 */
static inline void session_command_context_cleanup(session_t **ctx) {
   (void)ctx;
   session_set_command_context(NULL);
}

/**
 * @brief Scope guard macro for command context
 *
 * Sets the command context and ensures it's cleared when the current scope exits.
 * This prevents context leaks on early returns or exceptions.
 *
 * @param session Session to use for command context
 */
#define SESSION_SCOPED_COMMAND_CONTEXT(session)                                       \
   session_t *_scoped_ctx_##__LINE__                                                  \
       __attribute__((cleanup(session_command_context_cleanup), unused)) = (session); \
   session_set_command_context(session)
#else
/* Local-only mode: scope guard is a no-op */
static inline void session_command_context_cleanup(session_t **ctx) {
   (void)ctx;
}
#define SESSION_SCOPED_COMMAND_CONTEXT(session)                                       \
   session_t *_scoped_ctx_##__LINE__                                                  \
       __attribute__((cleanup(session_command_context_cleanup), unused)) = (session); \
   (void)_scoped_ctx_##__LINE__
#endif /* ENABLE_MULTI_CLIENT */

/* =============================================================================
 * Stub Implementations for Local-Only Mode (no network features)
 *
 * When ENABLE_WEBUI is disabled, session_manager.c
 * is not compiled. These inline stubs provide the minimal API needed by
 * code that calls session functions unconditionally.
 * ============================================================================= */

#ifndef ENABLE_MULTI_CLIENT

/* Stub: No sessions in local-only mode */
static inline session_t *session_get_command_context(void) {
   return NULL;
}

static inline void session_set_command_context(session_t *session) {
   (void)session;
}

static inline session_t *session_get(uint32_t session_id) {
   (void)session_id;
   return NULL;
}

static inline session_t *session_find_by_uuid(const char *uuid) {
   (void)uuid;
   return NULL;
}

/**
 * @brief Get local session for local-only mode (lazy initialization)
 *
 * Creates a static session with conversation history on first call.
 * This allows local-only builds to maintain conversation context.
 *
 * @return Pointer to static local session (never NULL after first call)
 */
static inline session_t *session_get_local(void) {
   static session_t local_stub = { 0 };
   static bool initialized = false;

   if (!initialized) {
      local_stub.session_id = LOCAL_SESSION_ID;
      local_stub.type = SESSION_TYPE_LOCAL;
      local_stub.client_fd = -1;
      local_stub.conversation_history = json_object_new_array();
      pthread_mutex_init(&local_stub.history_mutex, NULL);
      pthread_mutex_init(&local_stub.llm_config_mutex, NULL);
      llm_get_default_config(&local_stub.llm_config);
      initialized = true;
   }
   return &local_stub;
}

static inline int session_manager_init(void) {
   return 0;
}

static inline void session_manager_cleanup(void) {
}

static inline void session_cleanup_expired(void) {
}

static inline void session_check_idle_conversations(void) {
}

static inline int session_count(void) {
   return 0;
}

static inline int session_set_llm_config(session_t *session, const session_llm_config_t *config) {
   (void)session;
   (void)config;
   return 1; /* Not supported in local-only mode */
}

static inline void session_get_llm_config(session_t *session, session_llm_config_t *config) {
   (void)session;
   /* In local-only mode, use global defaults */
   if (config) {
      llm_get_default_config(config);
   }
}

/**
 * @brief Initialize session with system prompt (local-only mode)
 *
 * Clears existing history and adds the system message.
 * Provides conversation context for LLM in local-only builds.
 */
static inline void session_init_system_prompt(session_t *session, const char *system_prompt) {
   if (!session || !session->conversation_history || !system_prompt)
      return;

   /* Clear existing messages */
   size_t len = json_object_array_length(session->conversation_history);
   for (size_t i = len; i > 0; i--) {
      json_object_array_del_idx(session->conversation_history, i - 1, 1);
   }

   /* Add system message */
   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "role", json_object_new_string("system"));
   json_object_object_add(msg, "content", json_object_new_string(system_prompt));
   json_object_array_add(session->conversation_history, msg);
}

static inline void session_release(session_t *session) {
   (void)session;
}

static inline void session_retain(session_t *session) {
   (void)session;
}

static inline void session_manager_set_prompt_builder(session_prompt_builder_t fn) {
   (void)fn;
}

static inline void session_manager_set_local_prompt_builder(session_local_prompt_builder_t fn) {
   (void)fn;
}

static inline void session_manager_refresh_all_prompts(void) {
}

static inline void composed_prompt_free(composed_prompt_t *p) {
   /* Inlined free — non-MULTI_CLIENT builds still release any heap
    * allocations a builder may have produced.  Cannot delegate to
    * prompt_compose_free (its header includes session_manager.h, so
    * a back-include would be circular). */
   if (p == NULL)
      return;
   free(p->stable_prefix);
   p->stable_prefix = NULL;
   free(p->volatile_block);
   p->volatile_block = NULL;
}

static inline char *session_manager_compose_prompt_string(const composed_prompt_t *blocks) {
   (void)blocks;
   return NULL;
}

static inline char *session_manager_build_system_prompt_string(int user_id) {
   (void)user_id;
   return NULL;
}

static inline int session_dispatch_user_turn(session_t *session, const char *user_turn_text) {
   (void)session;
   (void)user_turn_text;
   return 0;
}

static inline void session_update_system_messages(session_t *session,
                                                  const char *stable_prefix,
                                                  const char *volatile_block) {
   (void)session;
   (void)stable_prefix;
   (void)volatile_block;
}

static inline void session_append_to_volatile_segment(session_t *session, const char *text) {
   (void)session;
   (void)text;
}

static inline char *session_get_system_prompt(session_t *session) {
   (void)session;
   return NULL;
}

static inline char *session_get_full_system_prompt(session_t *session) {
   (void)session;
   return NULL;
}

/* Phase 1f stubs — local-only build never has multi-session dedup state. */
static inline int session_injected_set_lookup_locked(const session_t *session,
                                                     const char *source_id,
                                                     const char *item_id,
                                                     injected_set_entry_t *out) {
   (void)session;
   (void)source_id;
   (void)item_id;
   (void)out;
   return 1; /* FAILURE — never a hit in stub mode */
}

static inline int session_injected_set_record_locked(session_t *session,
                                                     const char *source_id,
                                                     const char *item_id,
                                                     float score) {
   (void)session;
   (void)source_id;
   (void)item_id;
   (void)score;
   return 0;
}

static inline int session_injected_set_advance_turn_locked(session_t *session) {
   (void)session;
   return 0;
}

static inline void session_injected_set_clear(session_t *session) {
   (void)session;
}

static inline void session_set_dispatch_session(session_t *session) {
   (void)session;
}

static inline session_t *session_get_dispatch_session(void) {
   return NULL;
}

static inline int64_t session_get_last_user_msg_id(session_t *session) {
   (void)session;
   return 0;
}

#endif /* !ENABLE_MULTI_CLIENT */

#ifdef __cplusplus
}
#endif

#endif  // SESSION_MANAGER_H
