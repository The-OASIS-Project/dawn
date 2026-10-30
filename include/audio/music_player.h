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
 * Music player: plays a user's queue to one output (a browser stream, a
 * speaker).  The player opens tracks, decodes them and decides what plays
 * next; the output renders the audio and tells its listener what happened.
 */

#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio/audio_decoder.h"
#include "audio/music_nav.h"
#include "audio/music_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The most frames a read hands write(): the player's read buffer holds this
 *  many stereo frames */
#define MUSIC_PLAYER_READ_FRAMES_MAX 960

/** Why the audio stops being written */
typedef enum {
   MUSIC_HALT_PAUSE, /**< Paused: what's buffered is what comes next */
   MUSIC_HALT_FLUSH, /**< A seek, a skip or a stop: what's buffered is stale */
   MUSIC_HALT_IDLE,  /**< Nothing left to play */
} music_halt_t;

typedef enum {
   MUSIC_EVENT_STATE,    /**< What's playing changed */
   MUSIC_EVENT_POSITION, /**< About once a second while playing */
   MUSIC_EVENT_ERROR,
} music_event_type_t;

typedef enum {
   /** A track failed while playing, or the queue reached one that wouldn't
    *  open (skipped, or stopped after several) */
   MUSIC_ERROR_DECODE,
   /** The track the listener picked wouldn't open (playback stops) */
   MUSIC_ERROR_PLAYBACK,
   MUSIC_ERROR_UNREACHABLE, /**< Its server couldn't be reached */
} music_error_t;

typedef struct {
   music_event_type_t type;
   bool gapless;          /**< STATE: follows what's buffered (an advance, or a stop after one) */
   double position_sec;   /**< POSITION */
   uint32_t duration_sec; /**< POSITION: the track's, 0 if unknown */
   music_error_t error;   /**< ERROR */
   const char *message;   /**< ERROR: for the listener, valid only for the call */
} music_event_t;

/**
 * What a player calls on its output: each on the player's thread with no
 * player lock held, except notify (below).  An output may release its device
 * on any halt and reopen it on the next write: a seek halts and writes on,
 * and every run of the thread ends with halt(FLUSH).  None runs once
 * music_player_shutdown() has returned.
 */
typedef struct {
   /** A track starts; may block (a device open).  The player has checked it
    *  is mono or stereo.  gapless: a natural advance, its audio follows what's
    *  buffered.  Sets how many frames each read hands write(), 1 to
    *  MUSIC_PLAYER_READ_FRAMES_MAX.  FAILURE: this output can't render it.
    *  write() reads what this sets without a lock. */
   int (*track_begin)(void *ctx,
                      const audio_decoder_info_t *info,
                      bool gapless,
                      size_t *read_frames);
   /** Render frames at the source rate; pcm holds room for a stereo frame of
    *  each, so mono can be expanded in place.  Paces the player: over time it
    *  returns no faster than the listener takes the audio, rendered or not (it
    *  may run ahead to build a lead), and soon once playback is paused or
    *  stopped. */
   void (*write)(void *ctx, int16_t *pcm, size_t frames);
   /** The audio stops being written */
   void (*halt)(void *ctx, music_halt_t kind);
   /** Tell the listener; never takes the player's control lock.  Also called
    *  by a control request (music_player_notify_state/_error) on its own
    *  thread holding the control lock, possibly at the same time as the
    *  player's thread: it must be thread-safe. */
   void (*notify)(void *ctx, const music_event_t *ev);
} music_output_ops_t;

typedef struct music_player music_player_t;

/** Outcome of a track request, as music_player_await() reports it */
typedef enum {
   MUSIC_OPEN_PLAYING,     /**< Opened and installed */
   MUSIC_OPEN_FAILED,      /**< Couldn't be opened (the listener was told) */
   MUSIC_OPEN_UNREACHABLE, /**< Its server couldn't be reached (told) */
   MUSIC_OPEN_CANCELLED,   /**< A stop or a newer request replaced it */
   MUSIC_OPEN_PENDING,     /**< Still opening when the wait ended */
   MUSIC_OPEN_CLOSED,      /**< The player closed (its listener went) */
} music_open_result_t;

/** What music_player_resume_or_start() did */
typedef enum {
   MUSIC_RESUME_RESUMED,      /**< Was paused on a track: plays on */
   MUSIC_RESUME_STARTED,      /**< Started the queue's track at the player's place */
   MUSIC_RESUME_UNCHANGED,    /**< Already playing, or the queue is empty */
   MUSIC_RESUME_START_FAILED, /**< The player's thread couldn't start */
} music_resume_t;

/** What a listener shows: the player and its queue, read at one moment */
typedef struct {
   bool playing;
   bool paused;
   bool loading; /**< A remote track is downloading before it plays */
   double position_sec;
   audio_format_type_t source_format;
   uint32_t source_rate;
   /** What the player reports as playing (see music_player_current()); an
    *  empty path when nothing */
   music_queue_entry_t track;
   int queue_index; /**< Where the queue holds it, or -1 */
   int queue_length;
   bool shuffle;
   music_repeat_mode_t repeat_mode;
} music_player_status_t;

/**
 * @brief A player for a queue, rendering to an output
 *
 * Attaches its place to the queue (it follows other players' edits) and
 * starts no thread, so an output can be handed the player after this returns
 * (for music_player_interrupted()).  Takes over the caller's
 * music_queue_acquire() reference; on failure the caller keeps it.
 *
 * @return The player, or NULL if it couldn't be created
 */
music_player_t *music_player_create(const music_output_ops_t *ops,
                                    void *ctx,
                                    user_music_queue_t *uq);

/**
 * @brief Close the player for good: stop its thread
 *
 * Takes the control lock (ending an LLM tool's wait), stops the thread and
 * marks the player closed, so music_player_control_lock() refuses from then
 * on and no output function runs again: the output can go once this
 * returns.  Frees nothing; safe to call twice.  Never call it holding a lock
 * the output's functions take.
 */
void music_player_shutdown(music_player_t *p);

/** @brief Take another reference: the player stays allocated until released */
void music_player_ref(music_player_t *p);

/**
 * @brief Drop a reference; the last one frees the player (closes its
 *        decoders, detaches from and releases the queue)
 *
 * The last release must come after music_player_shutdown().  Release after
 * music_player_control_unlock(), never while holding a player lock.  NULL is
 * ignored.
 */
void music_player_release(music_player_t *p);

/** The player's queue */
user_music_queue_t *music_player_queue(const music_player_t *p);

/** Players whose thread is running, across all of them */
int music_player_running(void);

/**
 * @brief Take a player's control lock
 *
 * One control request at a time (each may stop the thread, swap the decoder
 * and start it again).  Outermost for the player: taken before queue_mutex
 * and any lock the output's notify takes.
 *
 * @param preempt True on the one thread that must never wait (a network service thread):
 *        a holder waiting in music_player_await() stops waiting and releases
 *        it.  False on other threads, which wait.
 * @return true with the lock held; false (not held) once the player is
 *         closed (music_player_shutdown())
 */
__attribute__((warn_unused_result)) bool music_player_control_lock(music_player_t *p, bool preempt);

/** @brief Release a player's control lock */
void music_player_control_unlock(music_player_t *p);

/* ---- Under the control lock -------------------------------------------- */

/**
 * @brief Open and play a track
 *
 * Stops the current track, records the request and starts the thread, which
 * opens it (downloading a remote track) and tells the listener the outcome.
 * Never blocks on the open.  Before returning it sets what a snapshot
 * reports: playing, position 0, the queue entry's duration, and loading for
 * a remote track.
 *
 * @param path The track's path
 * @param hint The queue position it was picked at (the copy nearest it is
 *        the one playing), or -1 for the player's place
 * @param ticket_out Optional: the request's ticket, for music_player_await()
 * @return SUCCESS, or FAILURE if the thread couldn't start
 */
int music_player_play(music_player_t *p, const char *path, int hint, uint32_t *ticket_out);

/**
 * @brief A bare play: resume a pause, else start the track at the player's
 *        place when nothing is playing
 *
 * @param ticket_out Optional: when it STARTED, the request's ticket for
 *        music_player_await()
 */
music_resume_t music_player_resume_or_start(music_player_t *p, uint32_t *ticket_out);

/**
 * @brief Move the player's place one step, as next/previous do
 *
 * Under shuffle, next walks forward through the play history and then picks
 * a random other track; previous walks back through it, and there is nothing
 * earlier once it runs out.  Otherwise the place moves by one, and wraps only
 * under repeat-all.  Stops the thread before moving, but only when a move
 * happens; play the new track with music_player_play(path, -1).
 *
 * @param path_out Receives the new track's path (path_size > 0)
 * @param title_out Optional: receives its title (NULL or title_size 0 to skip)
 * @return true if the place moved, false at the end (or start) of the queue,
 *         or with nothing earlier in the history
 */
bool music_player_step(music_player_t *p,
                       music_nav_dir_t dir,
                       char *path_out,
                       size_t path_size,
                       char *title_out,
                       size_t title_size);

/**
 * @brief Stop and unload the current track
 *
 * Leaves the player stopped at position 0 (not paused); the queue and the
 * player's place are untouched.
 */
void music_player_stop(music_player_t *p);

/**
 * @brief Remove the queue entry at a 0-based index
 *
 * Every attached player's place follows.  Removing the current track unloads
 * it, and if it was playing, starts the track that took its place.
 *
 * @param start_failed Out: that start failed (its thread couldn't start)
 * @return SUCCESS, or FAILURE if index is out of range
 */
int music_player_remove(music_player_t *p, int index0, bool *start_failed);

/**
 * @brief Stop playback and empty the queue (every player's place goes to 0)
 *
 * @param removed_out Optional: the queue's length before
 */
void music_player_clear_queue(music_player_t *p, int *removed_out);

/**
 * @brief Wait for a track request's outcome
 *
 * Returns early with MUSIC_OPEN_PENDING when a preempting request wants the
 * control lock (the open carries on).  Never call it on a thread that
 * preempts.
 */
music_open_result_t music_player_await(music_player_t *p, uint32_t ticket, int timeout_ms);

/**
 * @brief Tell the output's listener the player's state changed (a control
 *        request's own change)
 */
void music_player_notify_state(music_player_t *p);

/** @brief Tell the output's listener a control request failed */
void music_player_notify_error(music_player_t *p, music_error_t error, const char *message);

/* ---- Without the control lock ------------------------------------------ */

/**
 * @brief Pause or resume (the thread stops writing, the track stays loaded)
 *
 * A pause with nothing playing changes nothing.
 *
 * @return true if it changed anything: pausing a playing track, or resuming
 *         a paused one (state_mutex only, never the queue lock)
 */
bool music_player_set_paused(music_player_t *p, bool paused);

/**
 * @brief Seek the playing track; the thread applies it between reads
 *
 * @return true if queued (the listener gets a state event once applied),
 *         false with nothing to seek in (no track, or one still opening)
 */
bool music_player_seek(music_player_t *p, double position_sec);

/** @brief Read the player and its queue (takes queue_mutex) */
void music_player_snapshot(music_player_t *p, music_player_status_t *out);

/**
 * @brief What the player reports as playing, and where the queue holds it
 *
 * While a track plays or is paused: the track the player picked (another
 * player may have replaced or edited the shared queue since), at the queue
 * position holding it, or -1 when the queue no longer does.  Otherwise the
 * entry at the player's place.
 *
 * @pre the queue's queue_mutex held
 * @param track_out Receives a copy of it; an empty path when there is none
 * @return Its queue position, or -1
 */
int music_player_current(music_player_t *p, music_queue_entry_t *track_out);

/**
 * @brief The output should stop writing soon: paused, stopped, or the thread
 *        is stopping (output functions only)
 */
bool music_player_interrupted(music_player_t *p);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_PLAYER_H */
