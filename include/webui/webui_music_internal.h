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
#include "audio/music_nav.h"
#include "audio/music_player.h"
#include "audio/music_queue.h"
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

/** Opus output sample rate: the music stream is always 48 kHz stereo */
#define OPUS_SAMPLE_RATE 48000

/** Lowest source sample rate the stream plays.  The stream thread reads one Opus
 *  frame's worth of source audio at a time, so at 8 kHz that's 160 frames; below
 *  it the 48 kHz resampler's ratio outgrows its buffers. */
#define WEBUI_MUSIC_MIN_SOURCE_RATE 8000
/** Highest source sample rate the stream plays (8x 96 kHz).  A crafted file
 *  can declare any rate, and the resampler takes an int rate and ratio. */
#define WEBUI_MUSIC_MAX_SOURCE_RATE 768000

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
 * @brief A music player: one listener's playback of a shared queue
 *
 * Each player has its own place, decoder and thread; the queue is shared
 * per user, so several tabs see one queue while playing independently.
 */
struct music_player {
   /* Streaming state */
   atomic_bool streaming;       /**< True if the player's thread is active */
   atomic_bool stop_requested;  /**< Request the thread to stop */
   pthread_t stream_thread;     /**< The thread's handle */
   pthread_mutex_t state_mutex; /**< Protects non-atomic state */

   /** Serializes control requests (WebUI controls, queue actions, the LLM music
    *  tool), each of which may stop the thread, swap the decoder and start it
    *  again.  Two at once could free a decoder the other's new thread is
    *  reading, or join one thread twice.  Outermost: taken before
    *  s_conn_registry_mutex (queue broadcasts), queue_mutex and state_mutex;
    *  never held with s_music_teardown_mutex; the thread never takes it.
    *  Taken through music_player_control_lock(). */
   pthread_mutex_t control_mutex;
   /** Set by a preempting request waiting for control_mutex (one that stops
    *  or replaces playback): wakes an LLM tool call waiting for its track to
    *  open, which then releases the lock, so the WebSocket thread never waits
    *  on it.  Other requests don't take the lock. */
   atomic_bool control_cancel;

   /** A seek for the thread to apply between reads (state_mutex), so a seek
    *  never stops the thread.  Dropped when a track is installed or
    *  unloaded. */
   bool seek_pending;
   uint64_t seek_frames;

   /** A remote track is being downloaded before it can play (state_mutex) */
   bool loading;

   /** A track waiting for the thread to open it (state_mutex), set by a
    *  start request or by auto-advance; empty when none.  Cleared once
    *  resolved or by music_player_stop().  A stop that interrupts the open
    *  leaves it to whoever stopped the thread: a new request overwrites it,
    *  a stop drops it. */
   char pending_path[MUSIC_PATH_MAX];
   bool pending_auto;      /**< Report the outcome as a natural advance */
   uint32_t pending_seq;   /**< Ticket of the request the slot belongs to */
   uint32_t open_seq;      /**< Last ticket issued (state_mutex) */
   uint32_t open_done_seq; /**< Last ticket resolved (state_mutex) */
   /** The last ticket music_player_play() issued, and its outcome once
    *  resolved (state_mutex): kept apart from auto-advance tickets, so a
    *  track that follows can't hide what the request did */
   uint32_t request_seq;
   uint32_t request_done_seq;
   int request_result; /**< music_open_result_t for request_done_seq */
   /** Broadcast when a ticket resolves, and when a preempting request wants
    *  control_mutex (control_cancel), so a waiting LLM tool steps aside */
   pthread_cond_t open_cond;

   /** Remote-track prefetch (state_mutex): the track after the current one,
    *  already downloaded and opened by a helper thread the player's thread
    *  owns (started when a track starts, joined before the thread exits).
    *  Used when that path is opened next; closed otherwise. */
   audio_decoder_t *prefetched;
   char prefetch_path[MUSIC_PATH_MAX]; /**< What's prefetched or prefetching */
   /** What this player played: previous under shuffle walks back through
    *  it.  A shuffle pick planned for next is its forward entry, so
    *  auto-advance and next play what was prefetched.  Written under
    *  state_mutex; whatever syncs it with the queue or resolves its entries
    *  also holds queue_mutex, taken first. */
   music_history_t history;
   /** The auto-advance pick in the pending slot added a history entry (a new
    *  track, or a step onto one), so a failed open drops it (state_mutex) */
   bool history_fresh;
   /** The track this player picked, as queued then (what it reports as
    *  playing, see music_player_current), and the queue's replace_epoch at
    *  the time (state_mutex; set with queue_mutex held too).  After another
    *  player replaces the queue this one plays on, then moves on from the new
    *  queue's top.  Empty path when nothing is picked. */
   music_queue_entry_t track;
   uint32_t track_epoch;
   pthread_t prefetch_thread;
   bool prefetch_running;       /**< The player's thread only */
   atomic_bool prefetch_cancel; /**< Aborts the helper's download */

   /* Decoder */
   audio_decoder_t *decoder; /**< Current file decoder */

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
   music_queue_pos_t pos;            /**< Place in the queue, attached to it (queue_mutex) */
   unsigned int shuffle_seed;        /**< Per-player PRNG seed for rand_r() */

   /* Position update tracking */
   uint64_t last_position_update_ms;

   /* The output the thread renders to, and how many frames a read hands it
    * (set when a track starts; the player's thread) */
   const music_output_ops_t *ops;
   void *ops_ctx;
   size_t read_frames;
};

/**
 * @brief Per-connection music state: the WebUI output of the connection's
 *        player
 *
 * Stereo 48 kHz Opus to the browser's dedicated music socket, paced on the
 * client's buffer reports.
 */
typedef struct {
   music_player_t *player;

   /* Encoder */
   OpusEncoder *encoder;   /**< Opus encoder (quality-specific) */
   resampler_t *resampler; /**< Source rate -> 48kHz resampler */

   /* Resampling buffer - accumulates resampled samples for exact 960-frame encoding */
   int16_t *resample_accum;     /**< Accumulation buffer for resampled stereo samples */
   size_t resample_accum_size;  /**< Size of accumulation buffer in samples */
   size_t resample_accum_count; /**< Current sample count in buffer */

   /* The playing track's format, as track_begin gave it (player's thread) */
   uint32_t source_rate;
   uint8_t source_channels;

   /* Settings: read by any thread, applied to the encoder by the player's */
   _Atomic music_quality_t quality;
   _Atomic music_bitrate_mode_t bitrate_mode;

   /* Pending reconfiguration: the values first, then the flag (set by the
    * WebSocket thread, applied by the player's thread) */
   atomic_bool reconfigure_requested;
   _Atomic music_quality_t pending_quality;
   _Atomic music_bitrate_mode_t pending_bitrate_mode;

   /* Connection reference */
   ws_connection_t *conn;

   /* Pacing (player's thread only).  Kept across runs of the thread: a
    * natural advance carries the buffer estimate on, a halt resets it. */
   uint64_t stream_start_time;   /**< Real-time pacer's start, 0 to restart it */
   uint64_t frames_sent;         /**< Frames sent since stream_start_time */
   uint64_t est_buffered_us;     /**< Estimate of the client's buffered audio */
   uint64_t last_seen_report_ms; /**< Last client buffer report already used */
   bool was_closed_loop;         /**< The last frame was paced on a fresh report */
   unsigned pace_log_counter;    /**< Throttles the periodic pacing log */

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
   atomic_int write_ring_count;
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
 * Player internals (shared between webui_music.c and webui_music_track.c)
 * ============================================================================= */

/** What the thread does after moving on from a finished track */
typedef enum {
   MUSIC_ADVANCE_PENDING, /**< The next track is in the pending slot */
   MUSIC_ADVANCE_STOPPED, /**< Playback ended; the listener has been told */
   MUSIC_ADVANCE_EXIT,    /**< The thread was asked to stop */
} music_advance_t;

/**
 * @brief Pick the next track after the current one ended (player's thread)
 *
 * Puts it in the pending slot (the thread opens it next), or stops playback
 * at the end of the queue or after too many failures.
 *
 * @param p Player (decoder already closed and NULL)
 * @param track_failed The track that just ended did so on a decode error
 * @param failures In/out: tracks in a row that produced no audio
 * @return What happened
 */
music_advance_t music_player_advance(music_player_t *p, bool track_failed, int *failures);

/**
 * @brief Open the pending track, if any (player's thread)
 *
 * @param failures In/out: tracks in a row that produced no audio
 * @return true if a pending request was handled
 */
bool music_player_run_pending_open(music_player_t *p, int *failures);

/** @brief Cancel and join the prefetch helper, if running (player's thread) */
void music_player_prefetch_stop(music_player_t *p);

/** @brief Report an unresolved pending request as cancelled (thread exit) */
void music_player_cancel_pending_ticket(music_player_t *p);

/**
 * @brief Start the player's thread
 *
 * @pre control lock held
 * @return SUCCESS, or FAILURE if it couldn't start
 */
int music_player_start_thread(music_player_t *p);

/**
 * @brief Stop the player's thread and wait for it
 *
 * @pre control lock held (or the player is being destroyed)
 */
void music_player_stop_thread(music_player_t *p);

/* =============================================================================
 * WebUI music (shared between the webui_music*.c files)
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
 * @brief Send music_state, optionally tagged as a natural advance
 *
 * @param advance_auto Tag the frame advance:"auto" (a natural track change or
 *        end, which the client applies when its buffered audio has played)
 */
void webui_music_send_state_impl(ws_connection_t *conn,
                                 session_music_state_t *state,
                                 bool advance_auto);

/**
 * @brief Send error message to client
 */
void webui_music_send_error(ws_connection_t *conn, const char *code, const char *message);

/**
 * @brief A connection's music state, created on first use
 *
 * Creation happens under the same lock the music server's lazy init uses, so
 * the WebSocket handlers, the LLM tool and the music server can't each make
 * one.
 *
 * @return The state, or NULL if it couldn't be created
 */
session_music_state_t *webui_music_state_get_or_init(ws_connection_t *conn);

/** How long the LLM music tool waits for its track to open: past the Plex
 *  connect timeout, so "unreachable" and "won't open" are answered truthfully,
 *  without leaving a voice turn silent for a slow download */
#define WEBUI_MUSIC_TOOL_OPEN_WAIT_MS 15000

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

/**
 * @brief Append (or replace) queue tracks by path
 *
 * Resolves each path via music_db_get_by_path() BEFORE the queue lock, then
 * adds them (music_queue_add: dedups in append mode, persists).  Does NOT
 * start playback or send transport: the caller does.
 *
 * @param uq       The queue
 * @param paths    Array of file paths
 * @param n        Number of paths
 * @param replace  true: clear the queue first (play); false: append (enqueue)
 * @param outcomes Optional caller-allocated array of n entries for per-item result
 * @return SUCCESS, or FAILURE on invalid args / allocation failure
 */
int webui_music_queue_apply_paths(user_music_queue_t *uq,
                                  const char *const *paths,
                                  int n,
                                  bool replace,
                                  music_queue_outcome_t *outcomes);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_MUSIC_INTERNAL_H */
