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
 * WebUI Internal Header - Shared state and helpers for webui_*.c modules
 *
 * This header is NOT part of the public API. It exposes internal state
 * and helper functions shared between webui_server.c and split handler
 * modules (webui_http.c, webui_admin.c, webui_history.c, etc.).
 *
 * All modules including this header MUST be compiled into the same binary.
 * Do not expose this header to external code.
 */

#ifndef WEBUI_INTERNAL_H
#define WEBUI_INTERNAL_H

#include <json-c/json.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "auth/auth_db.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "webui/webui_server.h"
#include "webui/webui_turn_ref.h"

/* =============================================================================
 * lws Return Convention
 *
 * lws callbacks return -1 to close the connection — this is the lws API
 * contract, not a DAWN error code.  Sole exception to the no-negative-returns
 * rule.  Internal helpers that cascade up to lws callbacks use this constant
 * to document the intent.
 * ============================================================================= */
#define LWS_CLOSE_CONNECTION (-1)

/* =============================================================================
 * Request Supersession Macro (used by worker threads)
 * ============================================================================= */

/**
 * @brief Check if a request has been superseded and should abort
 *
 * A request aborts if:
 * 1. Cancellation was requested (Stop button, or session teardown) — NOT a mere
 *    client disconnect.  Post background-jobs Phase 1, a disconnected turn keeps
 *    generating and is persisted server-side, so `disconnected` alone no longer
 *    aborts; only `cancel_requested` does.
 * 2. A newer request was initiated (user sent a new message before this one
 *    completed) — request_generation moved on.
 *
 * Workers should check this before and after long operations (LLM calls, etc.)
 * to avoid processing stale/cancelled requests.
 *
 * @param session Pointer to session_t
 * @param expected_gen The request_generation captured when work was queued
 * @return true if request should be aborted, false if still valid
 */
#define REQUEST_SUPERSEDED(session, expected_gen) \
   (atomic_load(&(session)->cancel_requested) ||  \
    atomic_load(&(session)->request_generation) != (expected_gen))

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Internal Constants
 * ============================================================================= */

#define WS_SEND_BUFFER_SIZE 16384
#define HTTP_MAX_POST_BODY 4096
#define AUTH_COOKIE_MAX_AGE (24 * 60 * 60) /* 24 hours */
#define MAX_TOKEN_MAPPINGS 16
#define MODEL_CACHE_TTL 60 /* Cache refresh interval in seconds */

/* WebSocket text buffer limits */
#define WEBUI_TEXT_BUFFER_INITIAL_CAP 8192
#define WEBUI_TEXT_BUFFER_MAX_CAP                                                        \
   (8 * 1024 * 1024) /* 8MB: attached documents' text; an older client's base64 images[] \
                      * still arrive whole to be ignored rather than dropped */

/* =============================================================================
 * Per-WebSocket Connection Data
 * ============================================================================= */

typedef struct {
   struct lws *wsi;                             /* libwebsockets handle */
   session_t *session;                          /* Session manager reference (use
                                                 * conn_get_session/conn_set_session for
                                                 * cross-thread access) */
   char session_token[WEBUI_SESSION_TOKEN_LEN]; /* Reconnection token */
   bool session_was_reconnected; /* True iff this conn adopted its OWN existing session via a
                                  * reconnect token (vs a fresh/throwaway auto-create); emitted
                                  * as `reconnected` on the session frame so the client knows
                                  * whether to load_conversation (fresh) or just re-anchor. */
   uint8_t *audio_buffer;        /* Opus audio accumulation */
   size_t audio_buffer_len;
   size_t audio_buffer_capacity;
   bool in_binary_fragment; /* True if receiving fragmented binary frame */
   uint8_t binary_msg_type; /* Message type from first fragment */
   _Atomic bool
       use_opus; /* True if client supports Opus codec (atomic: set by LWS, read by worker) */
   _Atomic bool tts_enabled; /* True if TTS output enabled (atomic: set by LWS, read by worker) */
   _Atomic bool
       tool_step_origin; /* True if this client renders its OWN turn's tool steps from the
                          * tool_step frame (uniform-pill clients); server then includes it
                          * in its own tool_step fan. Default off = #3 origin-excluded
                          * behavior (stock www). Set by LWS at handshake, read by worker. */
   bool is_satellite;    /* True if this is a DAP2 satellite connection */
   bool logged_out;      /* Its login ended (webui_conn_end_login): closing, and
                          * nothing it sends is handled */
   bool client_noted;    /* Its `client` self-description was logged */

   /* Text message fragmentation support (for large JSON payloads) */
   char *text_buffer;      /* Accumulation buffer for fragmented text messages */
   size_t text_buffer_len; /* Current data length in text_buffer */
   size_t text_buffer_cap; /* Allocated capacity of text_buffer */

   /* Auth state (populated at WebSocket establishment from HTTP cookie) */
   bool authenticated;
   int auth_user_id;
   char auth_session_token[AUTH_TOKEN_LEN]; /* For DB re-validation */
   char username[AUTH_USERNAME_MAX];
   /* Session-keepalive (always-on): connection-time intent hint only, stored for
    * diagnostics. It is deliberately NOT READ by any renewal/authorization logic
    * (renewal reads the DB keepalive_enabled flag, set by the authenticated
    * session_keepalive_enable handler) — do not wire behavior off this field, and
    * a client that never sends the hint still gets keepalive via the enable msg. */
   bool session_keepalive;
   /* Note: is_admin NOT cached - re-validated from DB on each admin operation */

   /* Missed notification delivery: set once queued replay has been pushed to the
    * client so the delivery only happens on the first ready message per connection. */
   bool missed_notif_delivered;

   /* Watches-panel live gauge stream: while set, the 1 Hz heartbeat pushes a
    * compact watch_readings frame to this connection (opt-in via
    * watch_readings_subscribe, browsers only).  Plain bool — needs no teardown;
    * the connection's user data is zero-initialized and slot reuse re-clears it. */
   bool watch_readings_subscribed;

   /* Stocks panel push: while subscribed, the stocks refresher thread fans the
    * owner's portfolio snapshot to this connection on the active cadence.
    * stocks_ext_hours mirrors the panel's "Extended Hours" switch. Plain bools —
    * zero-initialized user data, re-cleared on slot reuse; no teardown needed. */
   bool stocks_subscribed;
   bool stocks_ext_hours;

   /* Watchlist panel push (independent of the portfolio subscription above): while
    * subscribed, the refresher fans stocks_watch_update (quotes for the user's
    * watched, not-held tickers) to this connection. stocks_watch_ext_hours opts this
    * connection into the extended-hours price overlay (aggregated per user; the
    * overlay is emitted only during an active pre/post window). */
   bool stocks_watch_subscribed;
   bool stocks_watch_ext_hours;

   /* Client IP address (captured at connection establishment for reliable logging) */
   char client_ip[64];

   /* Client counting: true once this connection has incremented s_client_count.
    * Ensures balanced decrement even if session is detached before disconnect. */
   bool counted;

   /* Active conversation tracking (for memory extraction on switch).
    * Atomic: written by the lws thread (new_conversation / load / reset), read by the
    * text worker thread (tool-turn persistence, focus injection, auto-compaction). */
   _Atomic int64_t active_conversation_id;
   bool active_conversation_private; /* If true, skip memory extraction */
   /* Private set before the conversation exists (set_private with no id): the
    * next conversation this connection creates is private from its first row.
    * Cleared when a conversation is bound.  Atomic: set on the lws thread, read
    * by a voice turn's auto-create on the audio worker.  When voice-only
    * surfaces get a way to go private first, this moves to session_t (their
    * conversations are created in session_voice_save, with no connection):
    * one flag, not two. */
   atomic_bool pending_private;

   /* Music streaming state (per-session, owned by webui_music.c) */
   void *music_state; /* session_music_state_t*, NULL if not initialized */

   /* Per-session music volume (0.0-1.0), synced with client.
    * Stored on connection (not music_state) because volume must be available
    * before music_subscribe and is an audio property of the connection.
    * Atomic: written by LLM tool thread and LWS thread, read by music stream thread. */
   _Atomic float volume;

   /* Always-on voice mode (per-connection, allocated on enable, freed on disable/disconnect).
    * NULL when always-on is not active. Owns per-connection VAD context, Opus decoder,
    * resampler, and circular audio buffer. */
   struct always_on_ctx *always_on;

   /* TTS audio pacing state (pacing fields written only from LLM worker thread,
    * not accessed by scheduler or LWS thread — no synchronization needed).
    * Note: ws_connection_t memory is owned by lws and remains valid until
    * server shutdown (not freed per-connection), so pointer access during
    * sleep is safe against use-after-free. */
   uint64_t tts_pace_start_us;  /* CLOCK_MONOTONIC when first audio sent (0 = not started) */
   uint64_t tts_audio_sent_us;  /* Cumulative audio duration sent (microseconds) */
   uint32_t tts_pace_stream_id; /* Stream ID for reset detection */
} ws_connection_t;

/**
 * @brief Atomically load conn->session (acquire semantics)
 *
 * Use at cross-thread boundaries where the maintenance thread may have
 * set conn->session to NULL via webui_detach_session(). Within the
 * single-threaded LWS callback context, direct conn->session access is fine
 * after an initial atomic load confirms non-NULL.
 */
/**
 * @brief Set the conversation the connection shows
 *
 * The one writer of conn->active_conversation_id: also keeps the session's copy
 * (session_t.viewed_conversation_id) that turn threads read.  Handler thread.
 */
void webui_conn_set_active_conversation(ws_connection_t *conn, int64_t conv_id);

/**
 * @brief Publish the connection's view to the session it just attached to
 *
 * Call wherever a connection takes a session (create, reconnect, re-anchor):
 * a new connection shows nothing yet, so a reconnected session must not keep the
 * evicted tab's conversation; one created for an open conversation shows it.
 */
void webui_conn_publish_view(ws_connection_t *conn);

/** Whether @p user_id may resume @p session (its owner, or no owner yet). */
bool webui_session_owned_by(const session_t *session, int user_id);

static inline session_t *conn_get_session(ws_connection_t *conn) {
   return __atomic_load_n(&conn->session, __ATOMIC_ACQUIRE);
}

/**
 * @brief Atomically store conn->session (release semantics)
 *
 * Use when writing conn->session from a non-LWS thread (e.g., maintenance
 * thread in webui_detach_session). Pairs with conn_get_session().
 */
static inline void conn_set_session(ws_connection_t *conn, session_t *s) {
   __atomic_store_n(&conn->session, s, __ATOMIC_RELEASE);
}

/* =============================================================================
 * HTTP Session Data
 * ============================================================================= */

/* Forward declarations for upload sessions */
struct http_image_session;
struct document_upload_session;

struct http_session_data {
   char path[256]; /* Request path */
   char post_body[HTTP_MAX_POST_BODY];
   size_t post_body_len;
   bool is_post;
   struct http_image_session *image_session;         /* For image uploads (NULL if not image) */
   struct document_upload_session *document_session; /* For doc uploads (NULL if not doc) */
   /* Dynamic body buffer for large POST endpoints (e.g., /api/documents/summarize) */
   char *large_body;      /* Dynamically allocated body (NULL if using post_body) */
   size_t large_body_len; /* Current length */
   size_t large_body_cap; /* Allocated capacity */
};

/* =============================================================================
 * Response Queue (worker -> WebUI thread)
 * ============================================================================= */

typedef struct {
   session_t *session;
   ws_response_type_t type;
   union {
      struct {
         char *state;
         char *detail;            /* Optional detail message */
         char *tools_json;        /* Optional JSON array of active tools */
         int64_t conversation_id; /* Turn's conversation (0 = none); routes the pill */
      } state;
      struct {
         char *role;
         char *text;
         bool server_saved;       /* Message already persisted to DB server-side */
         int64_t conversation_id; /* Live-turn conversation (0 = none); routes tool/visual frames */
         int64_t message_id; /* DB row id (0 = none); lets the client stamp data-message-id so a
                              * fanned-out message_appended dedups (server-authoritative §12c) */
         char client_ref[WEBUI_CLIENT_REF_MAX + 1]; /* the turn's client_ref ("" = none) */
      } transcript;
      struct {
         char *code;
         char *message;
         ws_error_severity_t severity;              /* Default 0 = WS_SEVERITY_ERROR */
         char client_ref[WEBUI_CLIENT_REF_MAX + 1]; /* the refused turn's client_ref ("" = none) */
      } error;
      struct {
         char *token;
      } session_token;
      struct {
         uint8_t *data;
         size_t len;
         bool is_opus; /* Codec used for this segment (for AUDIO_END marker) */
      } audio;
      struct {
         int current_tokens;
         int max_tokens;
         float threshold;
      } context;
      struct {
         uint32_t stream_id;
         int64_t conversation_id; /* Conversation this turn belongs to (0 = none) */
         char text[1024];         /* Buffer for delta/end text (increased for thinking) */
         bool will_persist;       /* stream_end only: server promises to persist this turn
                                     (Model A intent, SERVER_AUTHORITATIVE §6 step 2) */
      } stream;
      struct {
         char state[16];   /* idle, listening, thinking, speaking, error */
         int ttft_ms;      /* Time to first token (ms) */
         float token_rate; /* Tokens per second */
         int context_pct;  /* Context utilization 0-100 */
         int input_tokens; /* This turn's prompt tokens (last sub-call) — cache-rate denominator */
         int cached_tokens; /* Cache-read prompt tokens for this turn (last sub-call; 0 = miss) */
         int cache_write_tokens; /* Cache-write prompt tokens for this turn (0 when unreported) */
         int cache_saved_tokens; /* Provider-discounted net input tokens saved (may be negative) */
         char cache_state[16];   /* The last call's cache state ("" = none to report) */
         int64_t
             conversation_id; /* turn's conversation — client gates the footer to the active view */
      } metrics;
      struct {
         int tokens_before;
         int tokens_after;
         int messages_summarized;
         int level;
         char *summary;
         int64_t conversation_id; /* the compacted conversation; client gates on it */
      } compaction;
      struct {
         double position_sec;
         uint32_t duration_sec;
      } music_position;
      struct {
         char *json; /* Pre-serialized JSON string (heap-allocated) */
      } music_json;
      struct {
         char *json; /* Pre-serialized scheduler notification JSON */
      } scheduler_json;
      struct {
         char *json; /* Pre-serialized generic JSON (heap-allocated) */
      } generic_json;
   };
} ws_response_t;

/**
 * @brief Fan a background job's stream/thinking frame out to the browsers viewing
 *        its conversation.
 *
 * A job runs on a text-only job-pool session with no browser fd, so its stream
 * frames can't be queued to a single connection.  This re-targets a copy of the
 * frame to each of the owner's WEBUI connections whose ACTIVE conversation is the
 * job's, so a job you're watching streams like a normal conversation.  Called
 * from queue_response() for SESSION_TYPE_JOB responses.  Stream/thinking/reasoning
 * frames (inline text[]) and transcript frames (heap "tool"/"visual" debug entries,
 * the same native-tool path a normal turn uses) are fanned out; other types dropped.
 */
void webui_fanout_job_stream_response(ws_response_t *resp);

/* =============================================================================
 * Token-to-Session Mapping
 * ============================================================================= */

typedef struct {
   char token[WEBUI_SESSION_TOKEN_LEN];
   uint32_t session_id;
   time_t created;
   bool in_use;
} token_mapping_t;

/* =============================================================================
 * Discovery Cache (for model/interface scanning)
 * ============================================================================= */

typedef struct {
   struct json_object *models_response;     /* Cached list_models_response */
   struct json_object *interfaces_response; /* Cached list_interfaces_response */
   time_t models_cache_time;                /* When models were last scanned */
   time_t interfaces_cache_time;            /* When interfaces were last enumerated */
   pthread_mutex_t cache_mutex;             /* Protects cache access */
} discovery_cache_t;

/* =============================================================================
 * Extern Declarations for Module State (defined in webui_server.c)
 * ============================================================================= */

extern struct lws_context *s_lws_context;
extern _Atomic int s_running;
extern volatile int s_client_count;
extern int s_port;
extern char s_www_path[256];
extern char s_aurora_path[256];
extern pthread_mutex_t s_mutex;
extern pthread_rwlock_t s_config_rwlock;

/* Response queue */
extern ws_response_t s_response_queue[WEBUI_RESPONSE_QUEUE_SIZE];
extern int s_queue_head;
extern int s_queue_tail;
extern pthread_mutex_t s_queue_mutex;

/* Token mapping */
extern token_mapping_t s_token_map[MAX_TOKEN_MAPPINGS];
extern pthread_mutex_t s_token_mutex;

/* Discovery cache and allowed path prefixes are module-local in webui_config.c */

/* Send-side pipeline (queue + send_*_impl + audio sends) moved to
 * webui/webui_send.h.  Included below for backward compatibility with
 * webui_*.c modules that include webui_internal.h. */

/* =============================================================================
 * Active-connection registry (storage in webui_server.c)
 *
 * Exposed to sibling modules (webui_broadcasts.c) that iterate the
 * registry under s_conn_registry_mutex to fan messages out to all active
 * clients of a user.  Direct access is intentional — alternative would
 * require an iterator-callback API for every broadcast shape.
 * ============================================================================= */

#define MAX_ACTIVE_CONNECTIONS 64

extern ws_connection_t *s_active_connections[MAX_ACTIVE_CONNECTIONS];
extern pthread_mutex_t s_conn_registry_mutex;

/**
 * @brief Visitor invoked under s_conn_registry_mutex for each candidate connection.
 *
 * Return false to stop the walk early (existence checks); true to continue.  The
 * visitor runs WITH s_conn_registry_mutex held: it may queue_response (the
 * registry -> response-queue nesting is the established order) but MUST NOT
 * re-acquire s_conn_registry_mutex or block on TTS synthesis.
 */
typedef bool (*conn_visitor_fn)(ws_connection_t *conn, void *ctx);

/**
 * @brief Walk every authenticated connection for @user_id, invoking @visit.
 *
 * Bakes in ONLY the auth + user + excluded-origin filters (user_id <= 0 = all
 * users; exclude_session_id == 0 = exclude none).  All secondary predicates
 * (wsi liveness, session type, active conversation, satellite tier) belong in
 * the visitor.  Defined in webui_broadcasts.c; the §Phase-4 multi-target TTS
 * fan in webui_audio.c is the second caller.
 */
void for_each_user_conn(int user_id, uint32_t exclude_session_id, conn_visitor_fn visit, void *ctx);

/**
 * @brief Deliver queued missed scheduler notifications to a connection.
 *
 * Called after cookie-based auth completes at WebSocket open.  Reads up
 * to MISSED_NOTIF_DELIVERY_BATCH queued notifications for the user and
 * emits them as regular scheduler_notification messages with a "missed"
 * flag.  Defined in webui_broadcasts.c.
 */
void deliver_missed_notifications(ws_connection_t *conn);

/**
 * @brief Silent-observe event listener registered with llm_silent_observe().
 *
 * Fans the observation out to the silent-observer's owning user (or all
 * authenticated sessions for system-scoped events).  Defined in
 * webui_broadcasts.c, registered in webui_server_init().
 */
void webui_broadcast_silent_observation(const char *category,
                                        const char *note,
                                        int user_id,
                                        bool filter_match);

/**
 * @brief Central WebSocket JSON-message dispatcher.
 *
 * Parses one JSON message from the client, looks up the `type` field,
 * and routes to the appropriate `handle_*` helper.  Defined in
 * webui_message_dispatch.c; called from the LWS protocol callback in
 * webui_server.c on every text-frame receive.
 */
void handle_json_message(ws_connection_t *conn, const char *data, size_t len);

/**
 * @brief Handle a `text` message — text input from the user, with the images
 * attached to it by id (@p image_ids, @p image_id_count; persisted as
 * @p persist_content).  Defined in webui_server.c; called from
 * handle_json_message in webui_message_dispatch.c.
 */
void handle_text_message(ws_connection_t *conn,
                         const char *text,
                         size_t len,
                         const char image_ids[][IMAGE_ID_LEN],
                         int image_id_count,
                         const char *persist_content);

/**
 * @brief The `error` frame code and message for a turn whose images were
 *        refused: @p rc an IMAGE_REHYDRATE_ERR_* code (other values: the
 *        generic IMAGE_ERROR).  Both strings are static.  Defined in
 *        webui_text_processing.c.
 */
void webui_image_error_describe(int rc, const char **code_out, const char **message_out);

/**
 * @brief Handle a `get_metrics` message — emit the current session-metrics
 * snapshot to the client.  Defined in webui_server.c.
 */
void handle_get_metrics(ws_connection_t *conn);

/**
 * @brief Handle smart-home messages (Home Assistant family).
 *
 * Returns true if @p type matched a smart-home message and was
 * dispatched, false otherwise — caller continues the else-if chain.
 * Defined in webui_server.c.
 */
bool handle_smart_home_message(ws_connection_t *conn,
                               const char *type,
                               struct json_object *payload);

/**
 * @brief Queue init/state messages onto a freshly-authenticated connection.
 *
 * Defined in webui_send.c.  Called by the dispatcher in
 * webui_message_dispatch.c on session_init / login.
 */
void queue_init_messages(ws_connection_t *conn, const char *token);

/* Private-range (RFC 6455 4000-4999) WS close code sent to a connection whose
 * session a newer reconnect has taken over.  The client reads event.code === 4001
 * in onclose and backs off (does NOT auto-reconnect) instead of re-stealing the
 * session, which would ping-pong two tabs.  Kept in sync with www/js client. */
#define WEBUI_CLOSE_SUPERSEDED 4001

/* Private-range WS close code sent to a connection whose login ended (logout,
 * a revoke, a password change, expiry).  It follows a force_logout frame; the
 * client goes to the login page and does not reconnect.  Kept in sync with
 * www/js client. */
#define WEBUI_CLOSE_LOGGED_OUT 4002

/**
 * @brief Evict the connection currently owning @p existing so a newer reconnect
 *        can take over, closing it with WS code 4001 "superseded".
 *
 * No-op when @p existing has no owner, the owner IS @p new_conn, or the owner has
 * no live wsi.  The caller assigns `existing->client_data = new_conn` immediately
 * after; the evicted connection's LWS_CALLBACK_CLOSED then sees it is no longer the
 * owner (client_data != it) and leaves the session intact, only dropping its own
 * ref.  Must run on the lws service thread (reconnect dispatch).
 */
void webui_evict_session_owner(session_t *existing, ws_connection_t *new_conn);

/* ---- Per-turn persistence contract (webui_text_processing.c) ---------------
 * The tool-persist hook context + a scope that arms/disarms the WHOLE WebUI
 * turn-persistence contract as one unit, so a foreground turn path (typed or
 * voice) can't wire half of it. */
typedef struct {
   session_t *session; /* retained for the turn; NEVER a raw conn (UAF on disconnect) */
   int auth_user_id;   /* stable per connection; snapshotted at arm time */
} webui_tool_persist_ctx_t;

typedef struct {
   webui_tool_persist_ctx_t persist_ctx; /* stack-lifetime; referenced by the hook */
} webui_turn_persist_scope_t;

/* Arm {tool-persist hook, tool-iteration hook, will_persist_turn} together for the
 * synchronous LLM dispatch.  `scope` MUST outlive the dispatch; the hook reads the
 * conversation id live from session->stream_conversation_id, so bind the conversation
 * FIRST.  Captures only session + auth_user_id (never conn).  Arms nothing when
 * auth_user_id <= 0 (no user → nothing persistable).  Deliberately NOT for jobs
 * (own cb + snapshot conv, no will_persist) / messaging / research. */
void webui_turn_persist_arm(session_t *session,
                            int auth_user_id,
                            webui_turn_persist_scope_t *scope);
void webui_turn_persist_disarm(session_t *session, webui_turn_persist_scope_t *scope);

/* free_response moved to webui/webui_send.h */

/* =============================================================================
 * Token Mapping Functions
 * ============================================================================= */

/**
 * @brief Register a token->session_id mapping for reconnection
 *
 * Thread-safe. Evicts oldest if table is full.
 */
void register_token(const char *token, uint32_t session_id);

/**
 * @brief Remove all token mappings for a given session ID
 *
 * Thread-safe. Call when a session is destroyed to prevent stale lookups.
 */
void unregister_tokens_for_session(uint32_t session_id);

/**
 * @brief Look up session by reconnection token
 *
 * Thread-safe. Returns NULL if not found or session destroyed.
 * Cleans up stale entries when a mapped session no longer exists.
 */
session_t *lookup_session_by_token(const char *token);

/* WebSocket Send Helpers (send_json_message, send_binary_message,
 * send_state_impl(_full), send_audio_impl, send_audio_end_impl) moved to
 * webui/webui_send.h. */

/* =============================================================================
 * Path Security Helpers
 * ============================================================================= */

/**
 * @brief Get MIME type for file extension
 */
const char *get_mime_type(const char *path);

/**
 * @brief Check if path contains directory traversal patterns
 */
bool contains_path_traversal(const char *path);

/**
 * @brief Validate path is within www directory after symlink resolution
 */
bool is_path_within_www(const char *filepath, const char *www_path);

/* =============================================================================
 * HTTP Security Headers (defined in webui_http.c)
 * ============================================================================= */

/**
 * @brief Add security headers to an HTTP response being constructed
 *
 * Adds CSP, X-Frame-Options, X-Content-Type-Options, Referrer-Policy,
 * Permissions-Policy, and HSTS (when HTTPS is enabled).
 *
 * Must be called AFTER content-type/content-length headers but BEFORE
 * lws_finalize_http_header().
 *
 * @param wsi HTTP connection
 * @param p Pointer to current write position in header buffer
 * @param end Pointer to end of header buffer
 * @return SUCCESS on success, FAILURE on failure (buffer overflow)
 */
int webui_add_security_headers(struct lws *wsi, unsigned char **p, unsigned char *end);

/**
 * @brief Get pre-formatted security headers string for lws_serve_http_file()
 *
 * Returns a pointer to a static CRLF-separated header string built at init.
 * Used as the other_headers parameter for lws_serve_http_file().
 *
 * @param out_len If not NULL, receives the string length
 * @return Pointer to static header string (valid for lifetime of process)
 */
const char *webui_get_static_security_headers(int *out_len);

/**
 * @brief Initialize pre-formatted security headers string
 *
 * Must be called once during webui_server_init(), after g_config is loaded.
 */
void webui_security_headers_init(void);

/* =============================================================================
 * HTTP Protocol Handler (defined in webui_http.c)
 * ============================================================================= */

#ifdef ENABLE_AUTH
/**
 * @brief Check if HTTP request has valid session cookie
 *
 * @param wsi HTTP connection
 * @param session_out If not NULL, filled with session info on success
 * @return true if authenticated, false otherwise
 */
bool is_request_authenticated(struct lws *wsi, auth_session_t *session_out);
#endif

/**
 * @brief HTTP protocol callback for libwebsockets
 *
 * Handles static file serving, authentication endpoints, and OAuth callbacks.
 */
int callback_http(struct lws *wsi,
                  enum lws_callback_reasons reason,
                  void *user,
                  void *in,
                  size_t len);

/* =============================================================================
 * Session Token Generation
 * ============================================================================= */

/**
 * @brief Generate cryptographically secure session token
 *
 * @param token_out Buffer (WEBUI_SESSION_TOKEN_LEN bytes)
 * @return 0 on success, 1 on failure
 */
int generate_session_token(char token_out[WEBUI_SESSION_TOKEN_LEN]);

/* =============================================================================
 * Capability Helpers
 * ============================================================================= */

/**
 * @brief Check if client supports Opus codec
 *
 * Parses capabilities.audio_codecs array from init/reconnect payload.
 */
bool check_opus_capability(struct json_object *payload);

/* =============================================================================
 * Authentication Helpers
 * ============================================================================= */

/**
 * @brief Check if connection is a registered satellite session.
 *
 * Use this alongside conn_require_auth() at endpoints that satellites
 * should be allowed to access (e.g., music handlers). Check this FIRST
 * to avoid conn_require_auth()'s side-effect of sending an UNAUTHORIZED error.
 */
static inline bool conn_is_satellite_session(ws_connection_t *conn) {
   return conn && conn->is_satellite && conn->session != NULL;
}

/**
 * @brief Check if WebSocket connection is authenticated
 *
 * Re-validates session against database. Sends error if not authenticated.
 *
 * @param conn WebSocket connection
 * @return true if authenticated, false otherwise (error sent)
 */
bool conn_require_auth(ws_connection_t *conn);

/**
 * @brief Like conn_require_auth, but hands back the re-validated session.
 *
 * Lets a caller reuse the single DB read (e.g. handle_ping renewing keepalive)
 * instead of issuing a second get_session. @p session_out may be NULL.
 *
 * @param conn WebSocket connection
 * @param session_out Optional buffer to receive the validated session
 * @return true if authenticated with valid session, false otherwise (error sent)
 */
bool conn_require_auth_ex(ws_connection_t *conn, auth_session_t *session_out);

/**
 * @brief Check if WebSocket connection has admin privileges
 *
 * Re-validates is_admin from database. Sends error if not admin.
 *
 * @param conn WebSocket connection
 * @return true if admin, false otherwise (error sent)
 */
bool conn_require_admin(ws_connection_t *conn);

/**
 * @brief CSRF same-origin check (Origin/Referer host vs our Host header).
 *
 * Allows requests with no Origin/Referer (non-browser clients). Used for
 * state-changing REST endpoints and the WebSocket upgrade. Must be called while
 * the request headers are still present (during the HTTP request / WS handshake).
 *
 * @return true if same-origin (or no Origin/Referer), false if cross-origin.
 */
bool webui_is_same_origin_request(struct lws *wsi);

/**
 * @brief Build a conversation's LLM context for a turn, detached from any session
 *
 * session_turn_begin()'s loader (registered by webui_server_init).  Leaves every
 * session untouched; reports the conversation's stored LLM settings (over
 * @p base) in @p cfg_out / @p has_cfg_out.
 *
 * @return New history array (caller owns), or NULL
 */
json_object *webui_turn_history_loader(int user_id,
                                       int64_t conv_id,
                                       const session_llm_config_t *base,
                                       session_llm_config_t *cfg_out,
                                       bool *has_cfg_out);

/**
 * @brief Send JSON response to WebSocket client via the response queue
 *
 * Serializes the JSON object to a string and queues it as WS_RESP_JSON.
 * Safe to call from any context (RECEIVE callbacks, WRITEABLE callbacks,
 * or worker threads). The response is sent asynchronously via the
 * process_one_response() drain loop, ensuring one lws_write() per
 * WRITEABLE callback.
 *
 * The caller retains ownership of the json_object and must free it
 * after this call returns (the JSON string is copied internally).
 */
void send_json_response(ws_connection_t *conn, json_object *response);

/**
 * @brief Broadcast a JSON frame to one user's authenticated sessions.
 *
 * Fans @p root out to every authenticated connection whose auth_user_id matches
 * @p user_id (or all users when @p user_id <= 0).  TAKES OWNERSHIP of @p root and
 * frees it before the connection walk — the caller must not reuse or free it.
 *
 * @param user_id       Target user; <= 0 broadcasts to all authenticated users.
 * @param root          JSON frame (ownership transferred).
 * @param browsers_only Skip satellite connections when true.
 * @return Number of connections the frame was queued to.
 */
int webui_broadcast_json_to_user(int user_id, json_object *root, bool browsers_only);

/**
 * @brief Push a live watch_readings frame to every subscribed Watches panel.
 *
 * The 1 Hz heartbeat entry (driven from the dawn main loop beside attention_tick).
 * No-op unless proactive attention is enabled AND a browser has opted in via
 * watch_readings_subscribe — so an idle tick with the panel closed costs one
 * registry scan and takes no metric snapshot.  For each subscribed browser it
 * snapshots that user's watch readings and queues a compact
 * {type:"watch_readings", payload:{readings:[{id,has_current,current?}]}} frame.
 */
void webui_watch_readings_tick(void);

/**
 * @brief Broadcast a JSON frame to every admin user's browser sessions.
 *
 * Resolves the admin user_id set via auth_db BEFORE taking the connection
 * registry lock (never nests conn_registry -> auth_db), then walks the registry
 * once. TAKES OWNERSHIP of @p root. For admin-only data (e.g. HA realtime state
 * deltas) where there is no single acting user to scope to.
 *
 * @param browsers_only Skip satellite connections when true.
 * @return Number of connections the frame was queued to.
 */
int broadcast_json_to_admins(json_object *root, bool browsers_only);

/**
 * @brief Broadcast a "config_changed" nudge to every admin browser.
 *
 * Empty-payload signal ("something changed, re-fetch") emitted after a successful
 * set_config save so other admin sessions (a second WebUI tab, Aurora) refresh
 * their config-derived panels instead of showing a stale view until reconnect.
 * Config is daemon-global + admin-only, so it fans to all admins (browsers only).
 */
void webui_broadcast_config_changed(void);

/**
 * @brief Effective model name for a resolved LLM config (session model if set,
 *        else the provider/type default). Never empty for a valid provider.
 * @param resolved Resolved LLM config to read the model from.
 * @return Model-name string; not owned by the caller — copy before further calls.
 */
const char *webui_effective_model_name(const llm_resolved_config_t *resolved);

/**
 * @brief Stamp a conversation with the session's resolved LLM settings at
 *        creation, so no conversation persists NULL/empty llm columns. Fills
 *        only the columns that are currently NULL or empty via
 *        conv_db_fill_llm_settings_if_empty() — a per-column, race-proof update
 *        with NO message-count gate (so it is safe against the turn worker).
 * @param session Session whose resolved LLM config supplies the defaults.
 * @param conv_id Conversation to stamp.
 * @param user_id Owning user (for the auth_db lookup).
 */
void webui_conv_stamp_llm_settings(session_t *session, int64_t conv_id, int user_id);

/**
 * @brief Ensure the connection has a bound conversation, lazily creating one.
 *
 * Server-side mirror of the browser's `beginConversationBeforeSend`. Voice turns
 * (always-on wake-word and push-to-talk) are dispatched by the daemon, not by a
 * client "send", so they never trigger the client-side conversation pre-create.
 * A voice turn taken with nothing selected therefore ran against
 * `active_conversation_id == 0`, and the persistence gates on both voice paths
 * skipped the ASR user row AND the assistant reply — the exchange vanished on the
 * next reload. This binds (creating if needed) a conversation so those rows
 * persist. No-op returning the existing id when one is already bound, so it is
 * safe to call unconditionally on the voice paths.
 *
 * @param conn        Connection (must be authenticated).
 * @param title_hint  Text to auto-title from (the transcript); may be NULL.
 * @return The bound conversation id (existing or new), or 0 on failure.
 */
int64_t webui_ensure_active_conversation(ws_connection_t *conn, const char *title_hint);

/* Minimum word count for a voice transcript to auto-create a conversation.
 * Skips 1-2 word ASR fragments (false-wake / ambient-speech guard) so stray
 * wake-word turns don't mint durable, public, memory-extractable conversations. */
#define WEBUI_VOICE_AUTO_CONV_MIN_WORDS 3

/**
 * @brief True if a voice transcript is substantive enough to auto-create a
 *        conversation (>= WEBUI_VOICE_AUTO_CONV_MIN_WORDS whitespace-separated words).
 * @param text Transcript (may be NULL → false).
 */
bool webui_voice_transcript_substantive(const char *text);

/**
 * @brief Send an error message to a client (severity = error).
 * @param wsi     Target libwebsockets connection.
 * @param code    Machine-readable error code string.
 * @param message Human-readable message.
 */
void send_error_impl(struct lws *wsi, const char *code, const char *message);

/**
 * @brief Send an error/notice message with an explicit severity.
 * @param wsi      Target libwebsockets connection.
 * @param code     Machine-readable code string (e.g. INFO_THINKING_DISABLED).
 * @param message  Human-readable message.
 * @param severity info / warning / error — controls client-side routing.
 */
void send_error_impl_ex(struct lws *wsi,
                        const char *code,
                        const char *message,
                        ws_error_severity_t severity);

/**
 * @brief A persisted message fanned out to every browser of its user, the
 *        sender's own connection getting a copy that names its turn
 * @param origin     The session whose turn wrote it (its connection gets
 *                   payload.client_ref); NULL: every connection gets the same
 * @param client_ref That turn's client_ref; NULL or "" sends none
 * Otherwise as webui_broadcast_message_appended (conv_event.h).
 */
void webui_broadcast_message_appended_origin(int user_id,
                                             int64_t conv_id,
                                             int64_t msg_id,
                                             const char *role,
                                             const char *text,
                                             const char *reasoning,
                                             unsigned stream_id,
                                             const session_t *origin,
                                             const char *client_ref);

/**
 * @brief Send an error frame naming the text turn it refuses.
 * @param client_ref The turn's client_ref, echoed as payload.client_ref; NULL or
 *                   "" sends none (send_error_impl_ex uses the calling thread's
 *                   turn ref, webui_turn_ref_get()).
 */
void send_error_frame(struct lws *wsi,
                      const char *code,
                      const char *message,
                      ws_error_severity_t severity,
                      const char *client_ref);


/**
 * @brief Handle a phone_action WS message (answer / reject / hangup a call).
 *
 * Dispatches the blocking phone_service answer/hang-up onto a detached thread
 * so the lws service thread never blocks on the ECHO round-trip. Defined in
 * webui_phone.c. Caller must have auth-gated the connection.
 */
void webui_phone_handle_action(ws_connection_t *conn, const char *action);

/**
 * @brief Send the current active-call snapshot to one connection (reconnect
 * rehydration for the in-call panel). No-op if no call is active or the
 * connection isn't the modem owner. Defined in webui_phone.c.
 */
void webui_phone_send_status(ws_connection_t *conn);

/* =============================================================================
 * Ending logins (webui_login_sweep.c)
 * ============================================================================= */

/** @brief @p conn's owner key: its login cookie's public prefix, or "" (none). */
void webui_conn_owner_key(const ws_connection_t *conn, char out[SESSION_OWNER_KEY_LEN + 1]);

/** @brief A session just created for @p conn belongs to its login (if any). */
void webui_conn_own_session(ws_connection_t *conn, session_t *session);

/**
 * @brief May @p conn resume @p session (a reconnect token)?  Only a WebUI
 *        session, and only the one its own login created (an unowned one only
 *        for a connection that never logged in).
 */
bool webui_conn_may_resume(ws_connection_t *conn, session_t *session);

/**
 * @brief Attach @p conn to @p session unless the session is being destroyed.
 *        The caller's reference becomes the connection's.
 * @return false if it is being destroyed (the caller releases its reference).
 */
bool webui_conn_attach_session(ws_connection_t *conn, session_t *session);

/**
 * @brief End @p conn's login: force_logout, identity cleared, closed with
 *        WEBUI_CLOSE_LOGGED_OUT, and its session destroyed if that login
 *        owns it.  Service thread only.
 * @return true if it destroyed that session.
 */
bool webui_conn_end_login(ws_connection_t *conn, const char *reason);

/**
 * @brief Re-check @p conn's login in the database before giving it a session.
 *        A login that no longer exists is ended (webui_conn_end_login); a
 *        failed lookup sends an error.  Service thread only.
 * @return true if the login is valid.
 */
bool webui_conn_login_valid(ws_connection_t *conn);

/**
 * @brief Ask for a login sweep on the service thread: close the browser
 *        connections of logins the database no longer has, and destroy their
 *        sessions, connected or not.  Any thread; requests coalesce.  auth_db
 *        asks for one whenever it deletes a login (auth_sessions_changed).
 */
void webui_login_sweep_request(void);

/** @brief The service thread's wake: a requested sweep runs. */
void webui_login_sweep_run_pending(void);

/* =============================================================================
 * Connection Iterator (defined in webui_server.c, used by webui_music.c)
 * ============================================================================= */

/**
 * @brief Iterate all authenticated connections for a given user
 *
 * Calls callback for each active, authenticated connection with matching
 * auth_user_id and non-NULL music_state. Holds s_conn_registry_mutex
 * during iteration.
 *
 * @param user_id User ID to filter by (must be > 0)
 * @param callback Function to call for each matching connection
 * @param ctx Opaque context passed to callback
 */
void webui_for_each_conn_by_user(int user_id,
                                 void (*callback)(ws_connection_t *conn, void *ctx),
                                 void *ctx);

/**
 * @brief Collect authenticated connections for a given user into a caller-provided array
 *
 * Holds s_conn_registry_mutex only during collection, not during subsequent use.
 * Caller must use results promptly (pointers may become invalid on disconnect).
 *
 * @param user_id User ID to filter by (must be > 0)
 * @param out Array to fill with matching connection pointers
 * @param max_out Capacity of out array
 * @return Number of connections found (may exceed max_out; only max_out are stored)
 */
int webui_collect_conns_by_user(int user_id, ws_connection_t **out, int max_out);

/* Audio send wrappers (webui_send_audio, webui_send_audio_end,
 * webui_sentence_audio_callback) moved to webui/webui_send.h. */

#ifdef __cplusplus
}
#endif

/* Transitive includes for backward compatibility with the webui_*.c
 * modules that only included webui_internal.h.  Newer code should
 * include these directly where only the send-side or handler surface
 * is needed. */
#include "webui/webui_handlers.h"
#include "webui/webui_send.h"

#endif /* WEBUI_INTERNAL_H */
