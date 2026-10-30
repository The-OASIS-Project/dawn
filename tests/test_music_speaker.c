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
 * The local speaker over a fake playback device: when the device opens and
 * closes (a track's start, a pause, a seek, the end, a natural advance),
 * what's written to it (volume, ducking, a rate change), and how a device
 * that won't open or a short write is handled.  The audio backend's playback
 * calls and music_open_track() are replaced below; the player, queue and
 * converter are the real ones.
 */

#include <FLAC/stream_encoder.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio/audio_backend.h"
#include "audio/audio_decoder.h"
#include "audio/music_open.h"
#include "audio/music_queue.h"
#include "audio/music_speaker.h"
#include "config/dawn_config.h"
#include "dawn_error.h"
#include "unity.h"

#define DEVICE_RATE 44100 /* the converter's default output: no config */
#define TONE 8000

static char s_track_a[64];    /* stereo 44.1 kHz, 0.3 s */
static char s_track_mono[64]; /* mono 22.05 kHz, 0.3 s */
static char
    s_track_long[64]; /* stereo 44.1 kHz, 30 s: outlasts any test (the device runs at 10x) */
static char s_db_path[64];

/* Constant-amplitude tracks, so the written peak shows the volume */
static int write_flac(char *path, unsigned channels, unsigned rate, unsigned frames) {
   snprintf(path, 64, "/tmp/dawn_test_speakerXXXXXX.flac");
   int fd = mkstemps(path, 5);
   if (fd < 0) {
      return FAILURE;
   }
   close(fd);
   FLAC__StreamEncoder *enc = FLAC__stream_encoder_new();
   FLAC__int32 *pcm = calloc((size_t)frames * channels, sizeof(*pcm));
   bool ok = enc && pcm;
   if (ok) {
      FLAC__stream_encoder_set_channels(enc, channels);
      FLAC__stream_encoder_set_bits_per_sample(enc, 16);
      FLAC__stream_encoder_set_sample_rate(enc, rate);
      FLAC__stream_encoder_set_total_samples_estimate(enc, frames);
      ok = FLAC__stream_encoder_init_file(enc, path, NULL, NULL) ==
           FLAC__STREAM_ENCODER_INIT_STATUS_OK;
   }
   if (ok) {
      for (unsigned i = 0; i < frames * channels; i++) {
         pcm[i] = TONE;
      }
      ok = FLAC__stream_encoder_process_interleaved(enc, pcm, frames) &&
           FLAC__stream_encoder_finish(enc);
   }
   if (enc) {
      FLAC__stream_encoder_delete(enc);
   }
   free(pcm);
   if (!ok) {
      fprintf(stderr, "Can't write the fixture %s\n", path);
      unlink(path);
   }
   return ok ? SUCCESS : FAILURE;
}

/* ---- Stand-ins ------------------------------------------------------------ */

const dawn_config_t *config_get(void) {
   return NULL; /* the converter falls back to 44.1 kHz stereo */
}

audio_decoder_t *music_open_track(const char *path, const atomic_bool *cancel, bool *unreachable) {
   (void)cancel;
   if (unreachable) {
      *unreachable = false;
   }
   return strstr(path, "missing") ? NULL : audio_decoder_open(path);
}

audio_backend_type_t audio_backend_get_type(void) {
   return AUDIO_BACKEND_ALSA;
}

const char *audio_error_string(audio_error_t err) {
   (void)err;
   return "test error";
}

/* The fake device: counts, the written peak, and failures on demand */
static pthread_mutex_t s_dev_mutex = PTHREAD_MUTEX_INITIALIZER;
static int s_opens, s_drains, s_drops, s_closes, s_recovers;
static uint64_t s_frames;
static int s_peak;
static atomic_bool s_open_fails;
static atomic_int s_underruns_left; /* writes that underrun before succeeding */
static atomic_bool s_partial;       /* each write takes half */
static int s_handle_storage;

audio_stream_playback_handle_t *audio_stream_playback_open(const char *device,
                                                           const audio_stream_params_t *params,
                                                           audio_hw_params_t *hw) {
   (void)device;
   if (atomic_load(&s_open_fails)) {
      return NULL;
   }
   pthread_mutex_lock(&s_dev_mutex);
   s_opens++;
   pthread_mutex_unlock(&s_dev_mutex);
   hw->sample_rate = params->sample_rate;
   hw->channels = params->channels;
   hw->format = params->format;
   hw->period_frames = params->period_frames;
   hw->buffer_frames = params->buffer_frames;
   return (audio_stream_playback_handle_t *)&s_handle_storage;
}

ssize_t audio_stream_playback_write(audio_stream_playback_handle_t *h,
                                    const void *buffer,
                                    size_t frames) {
   (void)h;
   if (atomic_load(&s_underruns_left) > 0) {
      atomic_fetch_sub(&s_underruns_left, 1);
      return -(ssize_t)AUDIO_ERR_UNDERRUN;
   }
   size_t n = atomic_load(&s_partial) && frames > 1 ? frames / 2 : frames;
   const int16_t *pcm = buffer;
   pthread_mutex_lock(&s_dev_mutex);
   for (size_t i = 0; i < n * 2; i++) {
      int v = abs(pcm[i]);
      s_peak = v > s_peak ? v : s_peak;
   }
   s_frames += n;
   pthread_mutex_unlock(&s_dev_mutex);
   usleep((useconds_t)(n * 1000000ULL / DEVICE_RATE / 10)); /* 10x real time */
   return (ssize_t)n;
}

int audio_stream_playback_drain(audio_stream_playback_handle_t *h) {
   (void)h;
   pthread_mutex_lock(&s_dev_mutex);
   s_drains++;
   pthread_mutex_unlock(&s_dev_mutex);
   return 0;
}

int audio_stream_playback_drop(audio_stream_playback_handle_t *h) {
   (void)h;
   pthread_mutex_lock(&s_dev_mutex);
   s_drops++;
   pthread_mutex_unlock(&s_dev_mutex);
   return 0;
}

int audio_stream_playback_recover(audio_stream_playback_handle_t *h, int err) {
   (void)h;
   (void)err;
   pthread_mutex_lock(&s_dev_mutex);
   s_recovers++;
   pthread_mutex_unlock(&s_dev_mutex);
   return 0;
}

void audio_stream_playback_close(audio_stream_playback_handle_t *h) {
   (void)h;
   pthread_mutex_lock(&s_dev_mutex);
   s_closes++;
   pthread_mutex_unlock(&s_dev_mutex);
}

static int dev(const int *counter) {
   pthread_mutex_lock(&s_dev_mutex);
   int n = *counter;
   pthread_mutex_unlock(&s_dev_mutex);
   return n;
}

static uint64_t frames_written(void) {
   pthread_mutex_lock(&s_dev_mutex);
   uint64_t n = s_frames;
   pthread_mutex_unlock(&s_dev_mutex);
   return n;
}

/* Wait (up to 5 s) until the counter reaches want */
static bool wait_dev(const int *counter, int want) {
   for (int i = 0; i < 5000 && dev(counter) < want; i++) {
      usleep(1000);
   }
   return dev(counter) >= want;
}

static bool wait_frames(uint64_t than) {
   for (int i = 0; i < 5000 && frames_written() <= than; i++) {
      usleep(1000);
   }
   return frames_written() > than;
}

/* ---- Helpers -------------------------------------------------------------- */

static void reset_dev(void);

/* Each test plays for its own owner, so a fresh player replaces the last */
static int s_owner = 100;
static music_player_t *s_player;

static music_player_t *speaker_with(const char *a, const char *b) {
   s_player = music_speaker_acquire(++s_owner);
   TEST_ASSERT_NOT_NULL(s_player);
   reset_dev(); /* the last test's player has stopped: count only this one */
   user_music_queue_t *uq = music_player_queue(s_player);
   const char *paths[2] = { a, b };
   TEST_ASSERT_EQUAL_INT(SUCCESS, music_queue_apply_paths(uq, paths, b ? 2 : 1, true, NULL));
   return s_player;
}

static int play(music_player_t *p, const char *path, uint32_t *ticket) {
   TEST_ASSERT_TRUE(music_player_control_lock(p, true));
   int rc = music_player_play(p, path, 0, ticket);
   music_player_control_unlock(p);
   return rc;
}

static bool wait_stopped(music_player_t *p) {
   for (int i = 0; i < 500; i++) {
      music_player_status_t st;
      music_player_snapshot(p, &st);
      if (!st.playing) {
         return true;
      }
      usleep(10000);
   }
   return false;
}

/* music_queue_apply_paths() looks each path up in the library: these tests
 * have none, so the lookup answers from the fixtures */
bool music_path_is_valid(const char *path) {
   return path && path[0] == '/';
}

int music_db_get_by_path(const char *path, music_search_result_t *out, bool *found) {
   memset(out, 0, sizeof(*out));
   snprintf(out->path, sizeof(out->path), "%s", path);
   snprintf(out->title, sizeof(out->title), "Title of %s", path);
   *found = true;
   return SUCCESS;
}

static void reset_dev(void) {
   pthread_mutex_lock(&s_dev_mutex);
   s_opens = s_drains = s_drops = s_closes = s_recovers = 0;
   s_frames = 0;
   s_peak = 0;
   pthread_mutex_unlock(&s_dev_mutex);
}

void setUp(void) {
   reset_dev();
   atomic_store(&s_open_fails, false);
   atomic_store(&s_underruns_left, 0);
   atomic_store(&s_partial, false);
   setMusicVolume(1.0f);
   music_speaker_set_ducked(false);
}

void tearDown(void) {
   atomic_store(&s_open_fails, false); /* let a stuck track end */
   if (s_player) {
      music_player_release(s_player);
      s_player = NULL;
   }
}

/* ---- Tests ---------------------------------------------------------------- */

/* The device opens when the track starts and closes, drained, at the end */
static void test_opens_at_start_drains_at_end(void) {
   music_player_t *p = speaker_with(s_track_a, NULL);
   TEST_ASSERT_EQUAL_INT(SUCCESS, play(p, s_track_a, NULL));
   TEST_ASSERT_TRUE(wait_stopped(p));
   TEST_ASSERT_TRUE(wait_dev(&s_closes, 1));
   TEST_ASSERT_EQUAL_INT(1, dev(&s_opens));
   TEST_ASSERT_EQUAL_INT(1, dev(&s_drains));
   TEST_ASSERT_FALSE(music_speaker_is_playing());
   TEST_ASSERT_TRUE(frames_written() >= 13000);
}

/* A natural advance onto a track at another rate keeps the device open
 * (no gap) and converts the new track to the device's rate */
static void test_gapless_advance_keeps_device(void) {
   music_player_t *p = speaker_with(s_track_a, s_track_mono);
   TEST_ASSERT_EQUAL_INT(SUCCESS, play(p, s_track_a, NULL));
   TEST_ASSERT_TRUE(wait_stopped(p));
   TEST_ASSERT_TRUE(wait_dev(&s_closes, 1));
   TEST_ASSERT_EQUAL_INT(1, dev(&s_opens));
   /* 0.3 s + 0.3 s at the device's rate, give or take the resampler */
   uint64_t f = frames_written();
   TEST_ASSERT_TRUE(f > DEVICE_RATE * 55 / 100 && f < DEVICE_RATE * 65 / 100);
}

/* A pause drains and closes the device; a resume reopens it */
static void test_pause_closes_resume_reopens(void) {
   music_player_t *p = speaker_with(s_track_long, NULL);
   TEST_ASSERT_EQUAL_INT(SUCCESS, play(p, s_track_long, NULL));
   TEST_ASSERT_TRUE(wait_frames(0));
   TEST_ASSERT_TRUE(music_speaker_is_playing());

   TEST_ASSERT_TRUE(music_speaker_pause()); /* waits for the device to close */
   TEST_ASSERT_FALSE(music_speaker_is_playing());
   TEST_ASSERT_EQUAL_INT(1, dev(&s_drains));
   TEST_ASSERT_FALSE(music_speaker_pause()); /* nothing playing now */

   music_player_set_paused(p, false);
   TEST_ASSERT_TRUE(wait_dev(&s_opens, 2));
}

/* A seek drops what's buffered and reopens */
static void test_seek_drops_and_reopens(void) {
   music_player_t *p = speaker_with(s_track_long, NULL);
   TEST_ASSERT_EQUAL_INT(SUCCESS, play(p, s_track_long, NULL));
   TEST_ASSERT_TRUE(wait_frames(0));
   TEST_ASSERT_TRUE(music_player_seek(p, 0.5));
   TEST_ASSERT_TRUE(wait_dev(&s_drops, 1));
   TEST_ASSERT_TRUE(wait_dev(&s_opens, 2));
}

/* Switching the playback device moves playing music to it */
static void test_device_switch_reopens(void) {
   music_player_t *p = speaker_with(s_track_long, NULL);
   TEST_ASSERT_EQUAL_INT(SUCCESS, play(p, s_track_long, NULL));
   TEST_ASSERT_TRUE(wait_frames(0));
   music_speaker_set_device("other-device");
   TEST_ASSERT_TRUE(wait_dev(&s_drops, 1));
   TEST_ASSERT_TRUE(wait_dev(&s_opens, 2));
   music_speaker_set_device("test-device");
}

/* A device that won't open fails the track the listener picked */
static void test_device_wont_open_fails_track(void) {
   music_player_t *p = speaker_with(s_track_a, NULL);
   atomic_store(&s_open_fails, true);
   uint32_t ticket = 0;
   TEST_ASSERT_TRUE(music_player_control_lock(p, false));
   TEST_ASSERT_EQUAL_INT(SUCCESS, music_player_play(p, s_track_a, 0, &ticket));
   TEST_ASSERT_EQUAL_INT(MUSIC_OPEN_FAILED, music_player_await(p, ticket, 5000));
   music_player_control_unlock(p);
   TEST_ASSERT_TRUE(wait_stopped(p));
   TEST_ASSERT_EQUAL_UINT64(0, frames_written());
}

/* After a pause, a device that won't reopen keeps the player at playing
 * speed instead of racing through the queue in silence */
static void test_failed_reopen_paces(void) {
   music_player_t *p = speaker_with(s_track_long, NULL);
   TEST_ASSERT_EQUAL_INT(SUCCESS, play(p, s_track_long, NULL));
   TEST_ASSERT_TRUE(wait_frames(0));
   TEST_ASSERT_TRUE(music_speaker_pause());
   music_player_status_t before;
   music_player_snapshot(p, &before);

   atomic_store(&s_open_fails, true);
   music_player_set_paused(p, false);
   usleep(300000);
   music_player_status_t after;
   music_player_snapshot(p, &after);
   TEST_ASSERT_TRUE(after.playing);
   /* ~0.3 s of track in 0.3 s, not the rest of it */
   TEST_ASSERT_TRUE(after.position_sec - before.position_sec < 0.6);
}

/* A short write is finished and an underrun is recovered, so the track
 * plays out whole */
static void test_partial_and_underrun_retried(void) {
   music_player_t *p = speaker_with(s_track_a, NULL);
   atomic_store(&s_partial, true);
   atomic_store(&s_underruns_left, 1);
   TEST_ASSERT_EQUAL_INT(SUCCESS, play(p, s_track_a, NULL));
   TEST_ASSERT_TRUE(wait_stopped(p));
   TEST_ASSERT_TRUE(wait_dev(&s_closes, 1));
   TEST_ASSERT_EQUAL_INT(1, dev(&s_recovers));
   TEST_ASSERT_EQUAL_INT(1, dev(&s_opens));
   TEST_ASSERT_TRUE(frames_written() >= 13000);
}

/* The volume scales what's written; ducking lowers it further without
 * changing the set volume */
static void test_volume_and_ducking(void) {
   setMusicVolume(0.5f);
   music_speaker_set_ducked(true);
   music_player_t *p = speaker_with(s_track_a, NULL);
   TEST_ASSERT_EQUAL_INT(SUCCESS, play(p, s_track_a, NULL));
   TEST_ASSERT_TRUE(wait_stopped(p));
   TEST_ASSERT_TRUE(wait_dev(&s_closes, 1));
   int expect = (int)(TONE * 0.5f * 0.3f);
   TEST_ASSERT_INT_WITHIN(60, expect, dev(&s_peak));
   TEST_ASSERT_EQUAL_FLOAT(0.5f, getMusicVolume());
}

/* A new owner gets a player on their own queue; the old one closes */
static void test_owner_change_rebinds(void) {
   music_player_t *a = music_speaker_acquire(++s_owner);
   music_player_t *b = music_speaker_acquire(++s_owner);
   TEST_ASSERT_NOT_NULL(a);
   TEST_ASSERT_NOT_NULL(b);
   TEST_ASSERT_TRUE(a != b);
   TEST_ASSERT_TRUE(music_player_queue(a) != music_player_queue(b));
   TEST_ASSERT_FALSE(music_player_control_lock(a, false)); /* closed */
   music_player_release(a);
   music_player_release(b);
}

/* After cleanup there's no speaker (runs last) */
static void test_cleanup_ends_speaker(void) {
   music_speaker_cleanup();
   TEST_ASSERT_NULL(music_speaker_acquire(++s_owner));
}

int main(void) {
   audio_decoder_init();
   if (write_flac(s_track_a, 2, 44100, 13230) != SUCCESS ||
       write_flac(s_track_mono, 1, 22050, 6615) != SUCCESS ||
       write_flac(s_track_long, 2, 44100, 1323000) != SUCCESS) {
      return 1;
   }
   snprintf(s_db_path, sizeof(s_db_path), "/tmp/test_music_speaker_XXXXXX");
   int fd = mkstemp(s_db_path);
   if (fd < 0) {
      return 1;
   }
   close(fd);
   music_queue_init(s_db_path);
   music_speaker_set_device("test-device");

   UNITY_BEGIN();
   RUN_TEST(test_opens_at_start_drains_at_end);
   RUN_TEST(test_gapless_advance_keeps_device);
   RUN_TEST(test_pause_closes_resume_reopens);
   RUN_TEST(test_seek_drops_and_reopens);
   RUN_TEST(test_device_switch_reopens);
   RUN_TEST(test_device_wont_open_fails_track);
   RUN_TEST(test_failed_reopen_paces);
   RUN_TEST(test_partial_and_underrun_retried);
   RUN_TEST(test_volume_and_ducking);
   RUN_TEST(test_owner_change_rebinds);
   RUN_TEST(test_cleanup_ends_speaker);
   int rc = UNITY_END();

   music_queue_shutdown();
   char side[80];
   unlink(s_db_path);
   snprintf(side, sizeof(side), "%s-wal", s_db_path);
   unlink(side);
   snprintf(side, sizeof(side), "%s-shm", s_db_path);
   unlink(side);
   unlink(s_track_a);
   unlink(s_track_mono);
   unlink(s_track_long);
   audio_decoder_cleanup();
   return rc;
}
