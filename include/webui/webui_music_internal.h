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
 * WebUI Music Streaming - Internal Declarations
 *
 * This header contains internal types and functions shared between
 * webui_music.c and webui_music_handlers.c. Not part of the public API.
 */

#ifndef WEBUI_MUSIC_INTERNAL_H
#define WEBUI_MUSIC_INTERNAL_H

#include <libwebsockets.h>
#include <opus/opus.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "audio/audio_decoder.h"
#include "audio/music_db.h"
#include "audio/resampler.h"
#include "webui/webui_internal.h"
#include "webui/webui_music.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants (shared)
 * ============================================================================= */

/** Maximum Opus frame size */
#define OPUS_MAX_FRAME_SIZE 1276

/** Dedicated music-socket send ring depth (frames). At one 20ms frame per slot,
 *  8 slots absorb ~160ms of writable-callback / scheduling jitter before dropping.
 *  This depth is sized to cover the worst real-time stall on this path: the TTS
 *  output resample burst, ~140ms at RESAMPLER_QUALITY_MEDIUM (webui_audio.c). If
 *  that resampler is ever raised back to BEST (~680ms burst), or a slower core /
 *  longer sentence pushes the stall past ~160ms, this depth must be re-checked —
 *  otherwise the ring silently returns to dropping frames (watch write_drop_count). */
#define WEBUI_MUSIC_WRITE_RING 8

/* Closed-loop music buffer flow control (see music_stream_thread pacing in
 * webui_music.c). The server holds the client's worklet ring near a target depth
 * using periodic client buffer reports, bursting to refill after a drain. */

/** Target client buffer depth (ms) the server paces to hold. ~10x the ~200ms
 *  TTS-preemption stall it must ride out, and well under the client's 10s capacity. */
#define WEBUI_MUSIC_TARGET_BUFFER_MS 2000
/** Buffer reports older than this are stale → fall back to real-time pacing
 *  (old clients that never report, or a client whose report sender stalled). */
#define WEBUI_MUSIC_REPORT_STALE_MS 1000
/** During a refill burst, back off to real-time pacing once the send ring is at
 *  least this full (of WEBUI_MUSIC_WRITE_RING) so the burst can't overflow it. */
#define WEBUI_MUSIC_REFILL_RING_HIGH 6
/** Max single pacing sleep (us) — sanity bound against a bogus buffer estimate. */
#define WEBUI_MUSIC_MAX_PACE_SLEEP_US 100000
/* WEBUI_MUSIC_CLIENT_BUFFER_MAX_MS lives in the public webui_music.h — the music
 * server file clamps reports against it and does not include this internal header. */

/** Quality tier names for logging/UI */
extern const char *QUALITY_NAMES[MUSIC_QUALITY_COUNT];

/* =============================================================================
 * Internal Types
 * ============================================================================= */

/**
 * @brief Repeat mode for music playback
 */
typedef enum {
   MUSIC_REPEAT_NONE = 0,
   MUSIC_REPEAT_ALL = 1,
   MUSIC_REPEAT_ONE = 2,
} music_repeat_mode_t;

/**
 * @brief Queue entry for a music track
 */
typedef struct {
   char path[WEBUI_MUSIC_PATH_MAX];
   char title[WEBUI_MUSIC_STRING_MAX];
   char artist[WEBUI_MUSIC_STRING_MAX];
   char album[WEBUI_MUSIC_STRING_MAX];
   uint32_t duration_sec;
} music_queue_entry_t;

/**
 * @brief Shared per-user music queue
 *
 * Contains the queue entries and playback modes (shuffle/repeat) that
 * are shared across all browser tabs for the same authenticated user.
 * Protected by queue_mutex. For unauthenticated/satellite sessions,
 * each connection gets its own instance (not shared).
 */
typedef struct user_music_queue user_music_queue_t;
struct user_music_queue {
   int user_id;
   pthread_mutex_t queue_mutex;
   int ref_count;
   uint32_t generation; /**< Monotonic counter, incremented on every queue mutation */
   music_queue_entry_t queue[WEBUI_MUSIC_MAX_QUEUE];
   int queue_length;
   bool shuffle;
   music_repeat_mode_t repeat_mode;
};

/**
 * @brief Per-connection music streaming state
 *
 * Each WebSocket connection has its own playback state (position, decoder,
 * encoder, streaming thread). The queue itself is shared per-user via
 * shared_queue, allowing multiple tabs to see the same queue while
 * playing independently.
 */
typedef struct {
   /* Streaming state */
   atomic_bool streaming;       /**< True if streaming thread is active */
   atomic_bool stop_requested;  /**< Request streaming thread to stop */
   pthread_t stream_thread;     /**< Streaming thread handle */
   pthread_mutex_t state_mutex; /**< Protects non-atomic state */

   /* Thread synchronization for safe shutdown */
   pthread_cond_t decoder_idle_cond; /**< Signaled when decoder is not busy */
   atomic_bool shutdown_ack;         /**< Thread acknowledged shutdown request */

   /* Encoder */
   OpusEncoder *encoder;   /**< Opus encoder (quality-specific) */
   resampler_t *resampler; /**< Source rate -> 48kHz resampler */

   /* Resampling buffer - accumulates resampled samples for exact 960-frame encoding */
   int16_t *resample_accum;     /**< Accumulation buffer for resampled stereo samples */
   size_t resample_accum_size;  /**< Size of accumulation buffer in samples */
   size_t resample_accum_count; /**< Current sample count in buffer */

   /* Decoder */
   audio_decoder_t *decoder; /**< Current file decoder */
   atomic_bool decoder_busy; /**< True while decoder is being read */

   /* Playback state */
   bool playing;                      /**< True if playback in progress */
   bool paused;                       /**< True if paused */
   uint64_t position_frames;          /**< Current position in frames */
   uint32_t source_rate;              /**< Source file sample rate */
   uint8_t source_channels;           /**< Source file channels */
   audio_format_type_t source_format; /**< Source file format (FLAC, MP3, etc.) */
   uint32_t cached_duration_sec;      /**< Cached duration of current track (avoids queue_mutex) */

   /* Shared queue (per-user, or per-connection for unauthenticated) */
   user_music_queue_t *shared_queue; /**< Shared queue reference */
   int queue_index;                  /**< Current track in queue (per-session) */
   unsigned int shuffle_seed;        /**< Per-session PRNG seed for rand_r() */

   /* Settings */
   music_quality_t quality;
   music_bitrate_mode_t bitrate_mode;

   /* Pending reconfiguration (set by main thread, applied by streaming thread) */
   atomic_bool reconfigure_requested;
   music_quality_t pending_quality;
   music_bitrate_mode_t pending_bitrate_mode;

   /* Connection reference */
   ws_connection_t *conn;

   /* Position update tracking */
   uint64_t last_position_update_ms;

   /* Plex temp file (downloaded track, unlinked after decoder opens) */
   char temp_file[256]; /**< Path to Plex temp file (empty = none) */

   /* Dedicated music WebSocket (direct streaming).
    *
    * A small ring of pre-framed buffers, not a single slot: the music thread
    * produces one 20ms Opus frame every 20ms, and the writable callback drains
    * one frame per firing. A single slot dropped every frame that landed while a
    * write was still pending — so any scheduling hiccup (e.g. a heavy TTS
    * resample burst on another thread) turned into silent gaps. The ring absorbs
    * ~WEBUI_MUSIC_WRITE_RING * 20ms of jitter before it has to drop. */
   struct lws *music_wsi;       /**< Music server WebSocket (NULL if not connected) */
   pthread_mutex_t write_mutex; /**< Protects the write ring */
   uint8_t write_ring[WEBUI_MUSIC_WRITE_RING][LWS_PRE + 4 + 1276]; /**< per-slot: LWS_PRE + type
                                                                        byte + length prefix + max
                                                                        Opus frame */
   size_t write_ring_len[WEBUI_MUSIC_WRITE_RING]; /**< payload bytes in each slot */
   int write_ring_head; /**< next slot to drain (oldest) — only touched under write_mutex */
   int write_ring_tail; /**< next slot to fill — only touched under write_mutex */
   /* Occupied slots. Mutated under write_mutex (with head/tail), but ALSO read
    * unlocked by the streaming thread's pacer as a refill heuristic, so it is
    * atomic to keep that read race-free (TSan-clean). */
   _Atomic int write_ring_count;
   uint64_t write_drop_count; /**< total frames dropped when the ring was full */

   /* Closed-loop buffer flow control. Written by the music-server lws thread via
    * webui_music_report_buffer(); read by the per-session streaming thread's pacer.
    * Plain atomics (not write_mutex): independent scalars with no tie to the ring,
    * matching the existing lock-free control-flag pattern. calloc-zeroed, so 0 is
    * the natural "no report yet" sentinel. */
   atomic_uint client_buffered_ms;         /**< last client-reported worklet depth (ms) */
   _Atomic uint64_t last_buffer_report_ms; /**< get_time_ms() when it arrived (0 = none) */
} session_music_state_t;

/* =============================================================================
 * Internal Functions (shared between modules)
 * ============================================================================= */

/**
 * @brief Parse quality string to enum
 */
music_quality_t webui_music_parse_quality(const char *str);

/**
 * @brief Configure encoder for specified quality tier
 */
int webui_music_configure_encoder(session_music_state_t *state, music_quality_t quality);

/**
 * @brief Send current music state to client
 */
void webui_music_send_state(ws_connection_t *conn, session_music_state_t *state);

/**
 * @brief Send error message to client
 */
void webui_music_send_error(ws_connection_t *conn, const char *code, const char *message);

/**
 * @brief Start playback of a file
 */
int webui_music_start_playback(session_music_state_t *state, const char *path);

/**
 * @brief Stop streaming thread safely
 */
void webui_music_stop_streaming(session_music_state_t *state);

/**
 * @brief Check if path is within music library (security validation)
 */
bool webui_music_is_path_valid(const char *path);

/* =============================================================================
 * Shared State and Helpers (defined in webui_music.c, used by handlers)
 * ============================================================================= */

/**
 * @brief Start the streaming thread for a session.
 */
int webui_music_start_streaming(session_music_state_t *state);

/**
 * @brief Wait for the decoder to become idle.
 */
bool wait_decoder_idle(session_music_state_t *state, int timeout_ms);

/**
 * @brief Pick a random queue index different from the current one.
 *
 * Returns current_index if queue_length <= 1.
 */
int webui_music_pick_random_index(int current_index, int queue_length, unsigned int *shuffle_seed);

/**
 * @brief Broadcast queue state to all tabs for the same user
 *
 * Sends music_state update to all connections belonging to the same user,
 * except the excluded connection (which already got its update directly).
 *
 * @param uq Shared user queue (must NOT hold queue_mutex when calling)
 * @param exclude Connection to exclude from broadcast (may be NULL)
 */
void webui_music_broadcast_queue_state(user_music_queue_t *uq, ws_connection_t *exclude);

/**
 * @brief Execute volume tool for a WebUI session
 *
 * Called from volume_tool.c via session routing. Handles get/set actions
 * and syncs volume to the browser client via state update.
 */
char *webui_volume_execute_tool(ws_connection_t *conn,
                                const char *action,
                                const char *value,
                                int *should_respond);

/* =============================================================================
 * Shared queue-mutation helpers (transport-free)
 *
 * Mutate + persist + bump generation; the CALLER is responsible for transport
 * (send_json_response on the LWS thread, or webui_music_send_state +
 * webui_music_broadcast_queue_state on the LLM worker thread). This lets the
 * browser WebSocket handlers and the LLM tool path share one mutation body.
 * All honor the queue_mutex -> state_mutex lock order.
 * ============================================================================= */

/** Per-item outcome from webui_music_queue_apply_paths(). */
typedef enum {
   QUEUE_APPLY_ADDED = 0, /**< Resolved and appended */
   QUEUE_APPLY_DUPLICATE, /**< Already present in queue (append mode) */
   QUEUE_APPLY_NOT_FOUND, /**< Invalid path or not in the music DB */
   QUEUE_APPLY_FULL,      /**< Queue at WEBUI_MUSIC_MAX_QUEUE */
} queue_apply_outcome_t;

/**
 * @brief Append (or replace) queue tracks by path
 *
 * Resolves each path via music_db_get_by_path(), dedups against existing queue
 * contents (append mode), then mutates the shared queue + persists + bumps
 * generation under queue_mutex. DB lookups happen BEFORE the lock. Does NOT
 * start playback or send transport — the caller does.
 *
 * @param state    Session music state (uses state->shared_queue)
 * @param paths    Array of file paths
 * @param n        Number of paths
 * @param replace  true: clear the queue first (play); false: append (enqueue)
 * @param outcomes Optional caller-allocated array of n entries for per-item result
 * @return SUCCESS, or FAILURE on invalid args / allocation failure
 */
int webui_music_queue_apply_paths(session_music_state_t *state,
                                  const char *const *paths,
                                  int n,
                                  bool replace,
                                  queue_apply_outcome_t *outcomes);

/**
 * @brief Remove the queue entry at a 0-based index
 *
 * Shifts remaining entries, adjusts this session's and sibling sessions'
 * queue_index, persists. Transport-free.
 *
 * @return SUCCESS, or FAILURE if index is out of range
 */
int webui_music_queue_remove_index(session_music_state_t *state, int index0);

/**
 * @brief Clear the queue and reset playback for this session
 *
 * Stops streaming, zeroes the queue, resets queue_index/playing, persists.
 * Transport-free.
 *
 * @param removed_out Optional out: queue length before clearing
 * @return SUCCESS
 */
int webui_music_queue_clear(session_music_state_t *state, int *removed_out);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_MUSIC_INTERNAL_H */
