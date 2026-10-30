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
 * Music player internals: the player's state and the functions its two
 * source files share.  Only music_player.c and music_player_track.c (and the
 * player's tests) include this; everyone else uses audio/music_player.h.
 */

#ifndef MUSIC_PLAYER_INTERNAL_H
#define MUSIC_PLAYER_INTERNAL_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "audio/audio_decoder.h"
#include "audio/music_nav.h"
#include "audio/music_player.h"
#include "audio/music_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A music player: one listener's playback of a shared queue
 *
 * Each player has its own place, decoder and thread; the queue is shared
 * per user, so several players see one queue while playing independently.
 */
struct music_player {
   /* Streaming state */
   atomic_bool streaming;       /**< True if the player's thread is active */
   atomic_bool stop_requested;  /**< Request the thread to stop */
   pthread_t stream_thread;     /**< The thread's handle */
   pthread_mutex_t state_mutex; /**< Protects non-atomic state */

   /** Serializes control requests (a listener's controls, queue actions, an
    *  LLM tool), each of which may stop the thread, swap the decoder and start
    *  it again.  Two at once could free a decoder the other's new thread is
    *  reading, or join one thread twice.  Outermost for the player: taken
    *  before queue_mutex, state_mutex and any lock the output takes; the
    *  thread never takes it.  Taken through music_player_control_lock(). */
   pthread_mutex_t control_mutex;
   /** Preempting requests waiting for control_mutex (a count: a voice
    *  command, a WebUI control, a speaker rebind and a shutdown may overlap):
    *  wakes an LLM tool call waiting for its track to open, which then
    *  releases the lock, so a thread that preempts never waits on it. */
   atomic_int control_cancel;
   /** References (music_player_ref/release); the last release frees */
   atomic_int refs;
   /** Shut down for good (control_mutex): control_lock refuses */
   bool closed;
   /** A shutdown has begun: a preempted await reports cancelled */
   atomic_bool closing;

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

   /* The queue it plays (shared per user, or private) */
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

/* =============================================================================
 * Shared between music_player.c and music_player_track.c
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

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_PLAYER_INTERNAL_H */
