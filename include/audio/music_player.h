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
 * Music player: what a player hands its output (a browser stream, a
 * speaker).  The player decodes and decides what plays; the output renders
 * the audio and tells its listener what happened.
 */

#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio/audio_decoder.h"

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
   bool gapless;        /**< STATE: follows what's buffered (an advance, or a stop after one) */
   music_error_t error; /**< ERROR */
   const char *message; /**< ERROR: for the listener, valid only for the call */
} music_event_t;

/**
 * What a player calls on its output: each on the player's thread with no
 * player lock held.  An output may release its device on any halt and reopen
 * it on the next write: a seek halts and writes on, and every run of the
 * thread ends with halt(FLUSH).
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
   /** Tell the listener; never takes the player's control lock */
   void (*notify)(void *ctx, const music_event_t *ev);
} music_output_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_PLAYER_H */
