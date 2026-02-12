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
 * Music Playback Engine - Linear Opus decode to ALSA
 *
 * Decodes Opus frames from the music WebSocket and writes PCM directly to ALSA.
 * No ring buffer, no consumer thread — the LWS callback thread drives ALSA via
 * audio_playback_play_stereo(), which blocks until the hardware buffer drains.
 * ALSA's 170ms hardware buffer (8192 frames) absorbs network jitter.
 */

#include "music_playback.h"

#include <opus/opus.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "logging.h"

/* Max Opus frame: 960 samples per channel at 48kHz/20ms */
#define OPUS_MAX_FRAME_SAMPLES 960
#define OPUS_DECODE_BUF_SAMPLES (OPUS_MAX_FRAME_SAMPLES * 2) /* stereo */

struct music_playback {
   OpusDecoder *decoder;
   audio_playback_t *audio; /* Shared ALSA context (not owned) */
   atomic_int state;        /* music_pb_state_t */
   atomic_int volume;       /* 0-100, default 80 */
   atomic_int stop_flag;    /* Signals play_stereo to abort mid-write */
};

/* =============================================================================
 * Public API
 * ============================================================================= */

music_playback_t *music_playback_create(audio_playback_t *audio) {
   if (!audio || !audio->initialized) {
      LOG_ERROR("Music playback: audio context not initialized");
      return NULL;
   }

   music_playback_t *ctx = calloc(1, sizeof(music_playback_t));
   if (!ctx)
      return NULL;

   int opus_err;
   ctx->decoder = opus_decoder_create(48000, 2, &opus_err);
   if (opus_err != OPUS_OK || !ctx->decoder) {
      LOG_ERROR("Music playback: opus_decoder_create failed: %s", opus_strerror(opus_err));
      free(ctx);
      return NULL;
   }

   ctx->audio = audio;
   atomic_store(&ctx->state, MUSIC_PB_IDLE);
   atomic_store(&ctx->volume, 80);
   atomic_store(&ctx->stop_flag, 0);

   LOG_INFO("Music playback engine created (linear architecture)");
   return ctx;
}

void music_playback_destroy(music_playback_t *ctx) {
   if (!ctx)
      return;

   atomic_store(&ctx->stop_flag, 1);
   opus_decoder_destroy(ctx->decoder);
   free(ctx);

   LOG_INFO("Music playback engine destroyed");
}

int music_playback_push_opus(music_playback_t *ctx, const uint8_t *opus_data, int opus_len) {
   if (!ctx || !opus_data || opus_len <= 0)
      return -1;

   int st = atomic_load(&ctx->state);
   if (st == MUSIC_PB_PAUSED)
      return 0;

   /* Decode Opus frame to stack buffer */
   int16_t pcm[OPUS_DECODE_BUF_SAMPLES];
   int frames = opus_decode(ctx->decoder, opus_data, opus_len, pcm, OPUS_MAX_FRAME_SAMPLES, 0);
   if (frames <= 0) {
      LOG_WARNING("Music playback: opus_decode error: %s", opus_strerror(frames));
      return -1;
   }

   size_t n_samples = (size_t)frames * 2; /* stereo */

   /* Apply volume scaling (Q15 fixed-point, matching TTS path) */
   int vol = atomic_load(&ctx->volume);
   if (vol < 100) {
      int vol_fp = (vol << 15) / 100;
      for (size_t i = 0; i < n_samples; i++)
         pcm[i] = (int16_t)(((int32_t)pcm[i] * vol_fp) >> 15);
   }

   /* Write directly to ALSA — blocks until hardware buffer has space */
   audio_playback_play_stereo(ctx->audio, pcm, (size_t)frames, &ctx->stop_flag);

   /* First frame auto-starts playback */
   if (st == MUSIC_PB_IDLE)
      atomic_store(&ctx->state, MUSIC_PB_PLAYING);

   return frames;
}

void music_playback_stop(music_playback_t *ctx) {
   if (!ctx)
      return;

   atomic_store(&ctx->state, MUSIC_PB_IDLE);
   atomic_store(&ctx->stop_flag, 1);
   audio_playback_stop(ctx->audio);
   atomic_store(&ctx->stop_flag, 0);
   opus_decoder_ctl(ctx->decoder, OPUS_RESET_STATE);

   LOG_INFO("Music playback stopped");
}

void music_playback_flush(music_playback_t *ctx) {
   if (!ctx)
      return;

   atomic_store(&ctx->stop_flag, 1);
   audio_playback_stop(ctx->audio);
   atomic_store(&ctx->stop_flag, 0);
   opus_decoder_ctl(ctx->decoder, OPUS_RESET_STATE);
}

void music_playback_pause(music_playback_t *ctx) {
   if (!ctx)
      return;

   int st = atomic_load(&ctx->state);
   if (st != MUSIC_PB_PLAYING && st != MUSIC_PB_BUFFERING)
      return;

   atomic_store(&ctx->stop_flag, 1);
   atomic_store(&ctx->state, MUSIC_PB_PAUSED);
   audio_playback_stop(ctx->audio);
   atomic_store(&ctx->stop_flag, 0);

   LOG_INFO("Music playback paused");
}

void music_playback_resume(music_playback_t *ctx) {
   if (!ctx)
      return;

   if (atomic_load(&ctx->state) != MUSIC_PB_PAUSED)
      return;

   audio_playback_prepare(ctx->audio);
   atomic_store(&ctx->state, MUSIC_PB_PLAYING);

   LOG_INFO("Music playback resumed");
}

void music_playback_set_volume(music_playback_t *ctx, int volume) {
   if (!ctx)
      return;
   if (volume < 0)
      volume = 0;
   if (volume > 100)
      volume = 100;
   atomic_store(&ctx->volume, volume);
}

int music_playback_get_volume(music_playback_t *ctx) {
   return ctx ? atomic_load(&ctx->volume) : 0;
}

music_pb_state_t music_playback_get_state(music_playback_t *ctx) {
   return ctx ? (music_pb_state_t)atomic_load(&ctx->state) : MUSIC_PB_IDLE;
}

bool music_playback_is_playing(music_playback_t *ctx) {
   return ctx && atomic_load(&ctx->state) == MUSIC_PB_PLAYING;
}

int music_playback_get_buffered_ms(music_playback_t *ctx) {
   if (!ctx)
      return 0;

   long alsa_frames = audio_playback_get_delay_frames(ctx->audio);
   return (int)(alsa_frames * 1000 / 48000);
}
