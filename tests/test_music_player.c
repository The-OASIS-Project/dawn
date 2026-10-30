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
 * Music player over a fake output: what it hands the output (track starts,
 * writes, halts, events) for plays, natural advances, failures, remote
 * tracks, pause and seek, control-lock preemption and two players on one
 * queue.  music_open_track() is replaced below: local paths open real FLAC
 * files written in /tmp, plex: paths stand in for a remote server.
 */

#include <FLAC/stream_encoder.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audio/audio_decoder.h"
#include "audio/music_open.h"
#include "audio/music_player.h"
#include "audio/music_queue.h"
#include "dawn_error.h"
#include "unity.h"

/* ---- Fixtures ------------------------------------------------------------ */

static char s_track_a[64];    /* stereo 44.1 kHz, 0.3 s */
static char s_track_b[64];    /* stereo 44.1 kHz, 0.3 s */
static char s_track_mono[64]; /* mono 22.05 kHz, 0.3 s */
static char s_track_long[64]; /* stereo 48 kHz, 3 s */
static char s_db_path[64];

/* Runs before Unity starts, so it reports failure itself */
static int write_flac(char *path, unsigned channels, unsigned rate, unsigned frames) {
   snprintf(path, 64, "/tmp/dawn_test_playerXXXXXX.flac");
   int fd = mkstemps(path, 5);
   if (fd < 0) {
      fprintf(stderr, "Can't create a fixture in /tmp\n");
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
         pcm[i] = (FLAC__int32)((i % 200) * 100) - 10000; /* not silence */
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
      return FAILURE;
   }
   return SUCCESS;
}

/* ---- music_open_track() stand-in ----------------------------------------- */

/*
 * plex:/library/parts/ok/...   opens track A after a remote "download"
 * plex:/library/parts/down/... the server is unreachable
 * plex:/library/parts/slow/... waits until cancelled or s_release_slow
 * a path containing "missing"  won't open
 * anything else                opened as a local file
 */
static pthread_mutex_t s_open_mutex = PTHREAD_MUTEX_INITIALIZER;
static char s_open_paths[32][128];
static int s_open_counts[32];
static int s_prefetch_counts[32]; /* opens with no unreachable out: the prefetch's */
static atomic_bool s_release_slow;

static void count_open(const char *path, bool prefetch) {
   pthread_mutex_lock(&s_open_mutex);
   for (int i = 0; i < 32; i++) {
      if (s_open_paths[i][0] == '\0' || strcmp(s_open_paths[i], path) == 0) {
         snprintf(s_open_paths[i], sizeof(s_open_paths[i]), "%s", path);
         s_open_counts[i]++;
         s_prefetch_counts[i] += prefetch;
         break;
      }
   }
   pthread_mutex_unlock(&s_open_mutex);
}

static int opens_counted(const char *path, bool prefetch) {
   int n = 0;
   pthread_mutex_lock(&s_open_mutex);
   for (int i = 0; i < 32; i++) {
      if (strcmp(s_open_paths[i], path) == 0) {
         n = prefetch ? s_prefetch_counts[i] : s_open_counts[i];
      }
   }
   pthread_mutex_unlock(&s_open_mutex);
   return n;
}

static int opens_of(const char *path) {
   return opens_counted(path, false);
}

/* Wait (up to 5 s) until path has been opened; true if it was */
static bool wait_opened(const char *path) {
   for (int i = 0; i < 5000 && opens_of(path) == 0; i++) {
      usleep(1000);
   }
   return opens_of(path) > 0;
}

audio_decoder_t *music_open_track(const char *path, const atomic_bool *cancel, bool *unreachable) {
   count_open(path, unreachable == NULL);
   if (unreachable) {
      *unreachable = false;
   }
   if (strstr(path, "missing")) {
      return NULL;
   }
   if (strncmp(path, "plex:/library/parts/down/", 25) == 0) {
      if (unreachable) {
         *unreachable = true;
      }
      return NULL;
   }
   if (strncmp(path, "plex:/library/parts/slow/", 25) == 0) {
      while (!atomic_load(&s_release_slow)) {
         if (cancel && atomic_load(cancel)) {
            return NULL;
         }
         usleep(1000);
      }
      return audio_decoder_open(s_track_a);
   }
   if (strncmp(path, "plex:", 5) == 0) {
      return audio_decoder_open(s_track_a);
   }
   return audio_decoder_open(path);
}

/* ---- Fake output ---------------------------------------------------------- */

typedef enum {
   REC_BEGIN,
   REC_HALT,
   REC_EVENT
} rec_kind_t;

typedef struct {
   rec_kind_t kind;
   int sub;      /* halt kind, or event type */
   bool gapless; /* BEGIN, STATE */
   int error;    /* ERROR */
   unsigned rate;
   unsigned channels;
} rec_t;

#define REC_MAX 256

typedef struct {
   pthread_mutex_t mutex;
   pthread_cond_t cond;
   rec_t recs[REC_MAX];
   int nrecs;
   uint64_t frames_written;
   int speed;            /* write sleeps frames / rate / speed */
   unsigned refuse_rate; /* track_begin refuses tracks at this rate */
   size_t read_frames;   /* what track_begin asks for */
   unsigned rate;        /* the track begun last (player's thread) */
   unsigned channels;
   atomic_bool block_write;   /* write waits until the player is interrupted */
   atomic_bool write_blocked; /* a write is waiting */
   music_player_t *player;
} fake_output_t;

static fake_output_t s_out;

static void record(rec_t r) {
   pthread_mutex_lock(&s_out.mutex);
   if (s_out.nrecs < REC_MAX) {
      s_out.recs[s_out.nrecs++] = r;
   }
   pthread_cond_broadcast(&s_out.cond);
   pthread_mutex_unlock(&s_out.mutex);
}

static int fake_track_begin(void *ctx,
                            const audio_decoder_info_t *info,
                            bool gapless,
                            size_t *read_frames) {
   (void)ctx;
   record((rec_t){ .kind = REC_BEGIN,
                   .gapless = gapless,
                   .rate = info->sample_rate,
                   .channels = info->channels });
   if (info->sample_rate == s_out.refuse_rate) {
      return FAILURE; /* as an output whose device won't open */
   }
   s_out.rate = info->sample_rate;
   s_out.channels = info->channels;
   *read_frames = s_out.read_frames;
   return SUCCESS;
}

static void fake_write(void *ctx, int16_t *pcm, size_t frames) {
   (void)ctx;
   /* Expand mono in place, as an output may: the buffer holds a stereo frame
    * of each */
   if (s_out.channels == 1) {
      for (size_t i = frames; i-- > 0;) {
         pcm[i * 2] = pcm[i];
         pcm[i * 2 + 1] = pcm[i];
      }
   }
   while (atomic_load(&s_out.block_write) && !music_player_interrupted(s_out.player)) {
      atomic_store(&s_out.write_blocked, true);
      usleep(1000);
   }
   pthread_mutex_lock(&s_out.mutex);
   s_out.frames_written += frames;
   pthread_cond_broadcast(&s_out.cond);
   pthread_mutex_unlock(&s_out.mutex);
   usleep((useconds_t)(frames * 1000000ULL / s_out.rate / (unsigned)s_out.speed));
}

static void fake_halt(void *ctx, music_halt_t kind) {
   (void)ctx;
   record((rec_t){ .kind = REC_HALT, .sub = (int)kind });
}

static void fake_notify(void *ctx, const music_event_t *ev) {
   (void)ctx;
   if (ev->type == MUSIC_EVENT_POSITION) {
      return;
   }
   record(
       (rec_t){ .kind = REC_EVENT, .sub = ev->type, .gapless = ev->gapless, .error = ev->error });
}

static const music_output_ops_t s_ops = {
   .track_begin = fake_track_begin,
   .write = fake_write,
   .halt = fake_halt,
   .notify = fake_notify,
};

/* How many records match, from record index `from` on */
static int count_recs(int from, rec_kind_t kind, int sub) {
   int n = 0;
   for (int i = from; i < s_out.nrecs; i++) {
      if (s_out.recs[i].kind == kind && (sub < 0 || s_out.recs[i].sub == sub)) {
         n++;
      }
   }
   return n;
}

/* The records so far, copied under the lock (the player's thread adds more) */
static int copy_recs(rec_t *dst) {
   pthread_mutex_lock(&s_out.mutex);
   int n = s_out.nrecs;
   memcpy(dst, s_out.recs, sizeof(rec_t) * (size_t)n);
   pthread_mutex_unlock(&s_out.mutex);
   return n;
}

static int recs_matching(rec_kind_t kind, int sub) {
   pthread_mutex_lock(&s_out.mutex);
   int n = count_recs(0, kind, sub);
   pthread_mutex_unlock(&s_out.mutex);
   return n;
}

/* How many error events of one kind */
static int errors_of(music_error_t error) {
   int n = 0;
   pthread_mutex_lock(&s_out.mutex);
   for (int i = 0; i < s_out.nrecs; i++) {
      if (s_out.recs[i].kind == REC_EVENT && s_out.recs[i].sub == MUSIC_EVENT_ERROR &&
          s_out.recs[i].error == (int)error) {
         n++;
      }
   }
   pthread_mutex_unlock(&s_out.mutex);
   return n;
}

/* The first track start among n copied records */
static const rec_t *first_begin(const rec_t *r, int n) {
   for (int i = 0; i < n; i++) {
      if (r[i].kind == REC_BEGIN) {
         return &r[i];
      }
   }
   TEST_FAIL_MESSAGE("no track started");
   return NULL;
}

/* Wait (up to 5 s) until at least `want` records match; true if they did */
static bool wait_recs(rec_kind_t kind, int sub, int want) {
   struct timespec deadline;
   clock_gettime(CLOCK_REALTIME, &deadline);
   deadline.tv_sec += 5;
   pthread_mutex_lock(&s_out.mutex);
   bool ok;
   while (!(ok = count_recs(0, kind, sub) >= want)) {
      if (pthread_cond_timedwait(&s_out.cond, &s_out.mutex, &deadline) != 0) {
         ok = count_recs(0, kind, sub) >= want;
         break;
      }
   }
   pthread_mutex_unlock(&s_out.mutex);
   return ok;
}

/* Wait (up to 5 s) until the player reports it stopped */
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

static uint64_t frames_written(void) {
   pthread_mutex_lock(&s_out.mutex);
   uint64_t n = s_out.frames_written;
   pthread_mutex_unlock(&s_out.mutex);
   return n;
}

/* Wait (up to 5 s) until more than `than` frames are written; true if they were */
static bool wait_frames(uint64_t than) {
   for (int i = 0; i < 5000 && frames_written() <= than; i++) {
      usleep(1000);
   }
   return frames_written() > than;
}

/* ---- Queue helpers --------------------------------------------------------- */

static void queue_add(user_music_queue_t *uq, const char *path) {
   music_search_result_t t;
   memset(&t, 0, sizeof(t));
   snprintf(t.path, sizeof(t.path), "%s", path);
   snprintf(t.title, sizeof(t.title), "Title of %s", path);
   t.duration_sec = 1;
   pthread_mutex_lock(&uq->queue_mutex);
   TEST_ASSERT_EQUAL_INT(SUCCESS, music_queue_insert(uq, uq->queue_length, &t));
   pthread_mutex_unlock(&uq->queue_mutex);
}

/* The players a test made, and whether the test thread holds one's control
 * lock: a failed assertion leaves the test early, and tearDown frees them
 * before the queues go */
static music_player_t *s_live[2];
static bool s_held[2];

static int live_slot(music_player_t *p) {
   for (int i = 0; i < 2; i++) {
      if (s_live[i] == p) {
         return i;
      }
   }
   return -1;
}

static music_player_t *new_player(int user_id) {
   int slot = live_slot(NULL);
   TEST_ASSERT_TRUE(slot >= 0);
   user_music_queue_t *uq = music_queue_acquire(user_id);
   TEST_ASSERT_NOT_NULL(uq);
   music_player_t *p = music_player_create(&s_ops, NULL, uq);
   TEST_ASSERT_NOT_NULL(p);
   s_live[slot] = p;
   return p;
}

static void lock_control(music_player_t *p, bool preempt) {
   TEST_ASSERT_TRUE(music_player_control_lock(p, preempt));
   s_held[live_slot(p)] = true;
}

static void unlock_control(music_player_t *p) {
   s_held[live_slot(p)] = false;
   music_player_control_unlock(p);
}

static void free_player(music_player_t *p) {
   int slot = live_slot(p);
   if (s_held[slot]) {
      unlock_control(p);
   }
   s_live[slot] = NULL;
   atomic_store(&s_release_slow, true); /* a slow open must end for the join */
   music_player_shutdown(p);
   music_player_release(p);
}

/* Play a queue entry as a listener's control would */
static int play(music_player_t *p, const char *path, int hint, uint32_t *ticket) {
   lock_control(p, true);
   int rc = music_player_play(p, path, hint, ticket);
   unlock_control(p);
   return rc;
}

/* ---- Setup ------------------------------------------------------------------ */

void setUp(void) {
   pthread_mutex_lock(&s_out.mutex);
   s_out.nrecs = 0;
   s_out.frames_written = 0;
   s_out.speed = 10;
   s_out.refuse_rate = 0;
   s_out.read_frames = 480;
   pthread_mutex_unlock(&s_out.mutex);
   atomic_store(&s_out.block_write, false);
   atomic_store(&s_out.write_blocked, false);
   atomic_store(&s_release_slow, false);
   pthread_mutex_lock(&s_open_mutex);
   memset(s_open_paths, 0, sizeof(s_open_paths));
   memset(s_open_counts, 0, sizeof(s_open_counts));
   memset(s_prefetch_counts, 0, sizeof(s_prefetch_counts));
   pthread_mutex_unlock(&s_open_mutex);

   snprintf(s_db_path, sizeof(s_db_path), "/tmp/test_music_player_XXXXXX");
   int fd = mkstemp(s_db_path);
   TEST_ASSERT_TRUE(fd >= 0);
   close(fd);
   TEST_ASSERT_EQUAL_INT(SUCCESS, music_queue_init(s_db_path));
}

void tearDown(void) {
   for (int i = 0; i < 2; i++) {
      if (s_live[i]) {
         free_player(s_live[i]);
      }
   }
   music_queue_shutdown();
   char side[80];
   unlink(s_db_path);
   snprintf(side, sizeof(side), "%s-wal", s_db_path);
   unlink(side);
   snprintf(side, sizeof(side), "%s-shm", s_db_path);
   unlink(side);
}

/* ---- Tests ----------------------------------------------------------------- */

static rec_t r[REC_MAX]; /* a test's copy of the records */

/* A requested track opens, plays to its end, and the end of the queue stops
 * playback with a state event tagged as following what's buffered */
static void test_play_to_end_of_queue(void) {
   music_player_t *p = new_player(0);
   s_out.player = p;
   queue_add(music_player_queue(p), s_track_a);

   uint32_t ticket = 0;
   lock_control(p, false);
   TEST_ASSERT_EQUAL_INT(SUCCESS, music_player_play(p, s_track_a, 0, &ticket));
   TEST_ASSERT_EQUAL_INT(MUSIC_OPEN_PLAYING, music_player_await(p, ticket, 5000));
   unlock_control(p);

   TEST_ASSERT_TRUE(wait_stopped(p));
   TEST_ASSERT_TRUE(wait_recs(REC_HALT, MUSIC_HALT_IDLE, 1));
   TEST_ASSERT_EQUAL_INT(1, recs_matching(REC_BEGIN, -1));
   int n = copy_recs(r);
   TEST_ASSERT_FALSE(first_begin(r, n)->gapless); /* a request flushes */
   TEST_ASSERT_EQUAL_UINT(44100, first_begin(r, n)->rate);
   TEST_ASSERT_TRUE(frames_written() >= 13000); /* 0.3 s at 44.1 kHz */
   /* The last state event is the stop, tagged gapless */
   int last_state = -1;
   for (int i = 0; i < n; i++) {
      if (r[i].kind == REC_EVENT && r[i].sub == MUSIC_EVENT_STATE) {
         last_state = i;
      }
   }
   TEST_ASSERT_TRUE(last_state >= 0);
   TEST_ASSERT_TRUE(r[last_state].gapless);
   free_player(p);
}

/* A natural advance starts the next track gapless, with no halt between */
static void test_gapless_advance(void) {
   music_player_t *p = new_player(0);
   s_out.player = p;
   queue_add(music_player_queue(p), s_track_a);
   queue_add(music_player_queue(p), s_track_b);

   TEST_ASSERT_EQUAL_INT(SUCCESS, play(p, s_track_a, 0, NULL));
   TEST_ASSERT_TRUE(wait_recs(REC_BEGIN, -1, 2));
   int n = copy_recs(r);
   int begins = 0;
   int halts_between = 0;
   for (int i = 0; i < n && begins < 2; i++) {
      if (r[i].kind == REC_BEGIN) {
         begins++;
         if (begins == 2) {
            TEST_ASSERT_TRUE(r[i].gapless);
         }
      } else if (begins == 1 && r[i].kind == REC_HALT) {
         halts_between++;
      }
   }
   TEST_ASSERT_EQUAL_INT(0, halts_between);
   TEST_ASSERT_TRUE(wait_stopped(p));
   free_player(p);
}

/* The player hands a mono track to the output as mono, in a buffer with room
 * to expand it to stereo in place (ASan checks the room) */
static void test_mono_track(void) {
   music_player_t *p = new_player(0);
   s_out.player = p;
   s_out.read_frames = MUSIC_PLAYER_READ_FRAMES_MAX; /* expansion fills the buffer */
   queue_add(music_player_queue(p), s_track_mono);

   TEST_ASSERT_EQUAL_INT(SUCCESS, play(p, s_track_mono, 0, NULL));
   TEST_ASSERT_TRUE(wait_recs(REC_BEGIN, -1, 1));
   int n = copy_recs(r);
   TEST_ASSERT_EQUAL_UINT(1, first_begin(r, n)->channels);
   TEST_ASSERT_EQUAL_UINT(22050, first_begin(r, n)->rate);
   TEST_ASSERT_TRUE(wait_stopped(p));
   TEST_ASSERT_TRUE(frames_written() >= 6600);
   free_player(p);
}

/* A track the listener picked that won't open: a playback error, then stop */
static void test_requested_track_fails(void) {
   music_player_t *p = new_player(0);
   s_out.player = p;
   const char *bad = "/tmp/missing_track.flac";
   queue_add(music_player_queue(p), bad);

   uint32_t ticket = 0;
   lock_control(p, false);
   TEST_ASSERT_EQUAL_INT(SUCCESS, music_player_play(p, bad, 0, &ticket));
   TEST_ASSERT_EQUAL_INT(MUSIC_OPEN_FAILED, music_player_await(p, ticket, 5000));
   unlock_control(p);

   /* Stopped first, then told: wait for the event, not the stop */
   TEST_ASSERT_TRUE(wait_recs(REC_EVENT, MUSIC_EVENT_ERROR, 1));
   TEST_ASSERT_TRUE(wait_stopped(p));
   TEST_ASSERT_EQUAL_INT(1, errors_of(MUSIC_ERROR_PLAYBACK));
   TEST_ASSERT_EQUAL_INT(1, recs_matching(REC_EVENT, MUSIC_EVENT_ERROR));
   TEST_ASSERT_EQUAL_INT(0, recs_matching(REC_BEGIN, -1));
   free_player(p);
}

/* An advance onto a track that won't open skips it with one decode error */
static void test_advance_skips_failed_track(void) {
   music_player_t *p = new_player(0);
   s_out.player = p;
   queue_add(music_player_queue(p), s_track_a);
   queue_add(music_player_queue(p), "/tmp/missing_1.flac");
   queue_add(music_player_queue(p), s_track_b);

   TEST_ASSERT_EQUAL_INT(SUCCESS, play(p, s_track_a, 0, NULL));
   TEST_ASSERT_TRUE(wait_recs(REC_BEGIN, -1, 2));
   TEST_ASSERT_TRUE(wait_stopped(p));
   TEST_ASSERT_EQUAL_INT(1, opens_of("/tmp/missing_1.flac"));
   TEST_ASSERT_EQUAL_INT(1, opens_of(s_track_b));
   TEST_ASSERT_EQUAL_INT(1, errors_of(MUSIC_ERROR_DECODE));
   TEST_ASSERT_EQUAL_INT(1, recs_matching(REC_EVENT, MUSIC_EVENT_ERROR));
   free_player(p);
}

/* An output that refuses a track (a device that won't open): a pick the
 * listener made fails with a playback error */
static void test_output_refuses_requested_track(void) {
   music_player_t *p = new_player(0);
   s_out.player = p;
   s_out.refuse_rate = 22050;
   queue_add(music_player_queue(p), s_track_mono);

   uint32_t ticket = 0;
   lock_control(p, false);
   TEST_ASSERT_EQUAL_INT(SUCCESS, music_player_play(p, s_track_mono, 0, &ticket));
   TEST_ASSERT_EQUAL_INT(MUSIC_OPEN_FAILED, music_player_await(p, ticket, 5000));
   unlock_control(p);
   /* Stopped first, then told: wait for the event, not the stop */
   TEST_ASSERT_TRUE(wait_recs(REC_EVENT, MUSIC_EVENT_ERROR, 1));
   TEST_ASSERT_TRUE(wait_stopped(p));
   TEST_ASSERT_EQUAL_INT(1, errors_of(MUSIC_ERROR_PLAYBACK));
   TEST_ASSERT_EQUAL_UINT64(0, frames_written());
   free_player(p);
}

/* ...and an advance onto it skips it */
static void test_output_refuses_advanced_track(void) {
   music_player_t *p = new_player(0);
   s_out.player = p;
   s_out.refuse_rate = 22050;
   queue_add(music_player_queue(p), s_track_a);
   queue_add(music_player_queue(p), s_track_mono);
   queue_add(music_player_queue(p), s_track_b);

   TEST_ASSERT_EQUAL_INT(SUCCESS, play(p, s_track_a, 0, NULL));
   TEST_ASSERT_TRUE(wait_recs(REC_BEGIN, -1, 3)); /* a, the refused mono, b */
   TEST_ASSERT_TRUE(wait_stopped(p));
   TEST_ASSERT_EQUAL_INT(1, opens_of(s_track_b));
   TEST_ASSERT_EQUAL_INT(1, errors_of(MUSIC_ERROR_DECODE));
   free_player(p);
}

/* Five tracks in a row that won't open stop playback, and say so */
static void test_too_many_failures_stop(void) {
   music_player_t *p = new_player(0);
   s_out.player = p;
   queue_add(music_player_queue(p), s_track_a);
   char path[64];
   for (int i = 0; i < 7; i++) {
      snprintf(path, sizeof(path), "/tmp/missing_%d.flac", i);
      queue_add(music_player_queue(p), path);
   }

   TEST_ASSERT_EQUAL_INT(SUCCESS, play(p, s_track_a, 0, NULL));
   TEST_ASSERT_TRUE(wait_recs(REC_EVENT, MUSIC_EVENT_ERROR, 2));
   TEST_ASSERT_TRUE(wait_stopped(p));
   TEST_ASSERT_EQUAL_INT(1, opens_of("/tmp/missing_4.flac"));
   TEST_ASSERT_EQUAL_INT(0, opens_of("/tmp/missing_5.flac")); /* stopped before it */
   TEST_ASSERT_EQUAL_INT(2, errors_of(MUSIC_ERROR_DECODE));
   TEST_ASSERT_EQUAL_INT(2, recs_matching(REC_EVENT, MUSIC_EVENT_ERROR));
   free_player(p);
}

/* An unreachable server stops playback when every other queued track is
 * from it too */
static void test_unreachable_stops_all_remote_queue(void) {
   music_player_t *p = new_player(0);
   s_out.player = p;
   queue_add(music_player_queue(p), "plex:/library/parts/ok/1.flac");
   queue_add(music_player_queue(p), "plex:/library/parts/down/2.flac");
   queue_add(music_player_queue(p), "plex:/library/parts/down/3.flac");

   TEST_ASSERT_EQUAL_INT(SUCCESS, play(p, "plex:/library/parts/ok/1.flac", 0, NULL));
   TEST_ASSERT_TRUE(wait_recs(REC_EVENT, MUSIC_EVENT_ERROR, 1));
   TEST_ASSERT_TRUE(wait_stopped(p));
   TEST_ASSERT_EQUAL_INT(0, opens_of("plex:/library/parts/down/3.flac"));
   TEST_ASSERT_EQUAL_INT(1, errors_of(MUSIC_ERROR_UNREACHABLE));
   TEST_ASSERT_EQUAL_INT(1, recs_matching(REC_EVENT, MUSIC_EVENT_ERROR));
   free_player(p);
}

/* With a local track queued too, an unreachable server's track is skipped */
static void test_unreachable_skips_in_mixed_queue(void) {
   music_player_t *p = new_player(0);
   s_out.player = p;
   queue_add(music_player_queue(p), s_track_a);
   queue_add(music_player_queue(p), "plex:/library/parts/down/2.flac");
   queue_add(music_player_queue(p), s_track_b);

   TEST_ASSERT_EQUAL_INT(SUCCESS, play(p, s_track_a, 0, NULL));
   TEST_ASSERT_TRUE(wait_recs(REC_BEGIN, -1, 2));
   TEST_ASSERT_TRUE(wait_stopped(p));
   TEST_ASSERT_EQUAL_INT(1, opens_of(s_track_b));
   TEST_ASSERT_EQUAL_INT(1, errors_of(MUSIC_ERROR_DECODE)); /* skipped, not stopped */
   TEST_ASSERT_EQUAL_INT(0, errors_of(MUSIC_ERROR_UNREACHABLE));
   free_player(p);
}

/* The next remote track is prefetched: opened once, by the prefetch */
static void test_remote_prefetch_opens_once(void) {
   music_player_t *p = new_player(0);
   s_out.player = p;
   queue_add(music_player_queue(p), "plex:/library/parts/ok/1.flac");
   queue_add(music_player_queue(p), "plex:/library/parts/ok/2.flac");

   TEST_ASSERT_EQUAL_INT(SUCCESS, play(p, "plex:/library/parts/ok/1.flac", 0, NULL));
   TEST_ASSERT_TRUE(wait_recs(REC_BEGIN, -1, 2));
   TEST_ASSERT_TRUE(wait_stopped(p));
   TEST_ASSERT_EQUAL_INT(1, opens_of("plex:/library/parts/ok/2.flac"));
   TEST_ASSERT_EQUAL_INT(1, opens_counted("plex:/library/parts/ok/2.flac", true));
   free_player(p);
}

/* A pause halts the output and stops writes; resuming writes on */
static void test_pause_and_resume(void) {
   music_player_t *p = new_player(0);
   s_out.player = p;
   s_out.speed = 1; /* real time: 3 s of audio */
   queue_add(music_player_queue(p), s_track_long);

   TEST_ASSERT_EQUAL_INT(SUCCESS, play(p, s_track_long, 0, NULL));
   TEST_ASSERT_TRUE(wait_frames(0)); /* writing: past the pause check */
   music_player_set_paused(p, true);
   TEST_ASSERT_TRUE(wait_recs(REC_HALT, MUSIC_HALT_PAUSE, 1));
   TEST_ASSERT_TRUE(music_player_interrupted(p));
   uint64_t paused_at = frames_written();
   usleep(50000);
   TEST_ASSERT_EQUAL_UINT64(paused_at, frames_written());

   music_player_set_paused(p, false);
   TEST_ASSERT_FALSE(music_player_interrupted(p));
   TEST_ASSERT_TRUE(wait_frames(paused_at));
   free_player(p);
}

/* A seek flushes the output and reports the new position */
static void test_seek_flushes(void) {
   music_player_t *p = new_player(0);
   s_out.player = p;
   s_out.speed = 1;
   queue_add(music_player_queue(p), s_track_long);

   TEST_ASSERT_EQUAL_INT(SUCCESS, play(p, s_track_long, 0, NULL));
   TEST_ASSERT_TRUE(wait_frames(0)); /* installed, and its own state event sent */
   int states_before = recs_matching(REC_EVENT, MUSIC_EVENT_STATE);
   TEST_ASSERT_TRUE(music_player_seek(p, 0.8)); /* within the queued 1 s duration */
   TEST_ASSERT_TRUE(wait_recs(REC_HALT, MUSIC_HALT_FLUSH, 1));
   TEST_ASSERT_TRUE(wait_recs(REC_EVENT, MUSIC_EVENT_STATE, states_before + 1));
   music_player_status_t st;
   music_player_snapshot(p, &st);
   TEST_ASSERT_TRUE(st.position_sec >= 0.8);
   TEST_ASSERT_FALSE(music_player_seek(p, -1.0)); /* out of range: refused */
   TEST_ASSERT_EQUAL_INT(1, recs_matching(REC_HALT, MUSIC_HALT_FLUSH));
   free_player(p);
}

typedef struct {
   music_player_t *p;
   uint32_t ticket;
   music_open_result_t result;
   atomic_bool requested; /**< Holds the control lock, its track requested */
} waiter_t;

/* An LLM tool's request: the control lock without preempting, play, await */
static void *tool_request(void *arg) {
   waiter_t *w = arg;
   if (!music_player_control_lock(w->p, false)) {
      return NULL;
   }
   music_player_play(w->p, "plex:/library/parts/slow/1.flac", 0, &w->ticket);
   atomic_store(&w->requested, true);
   w->result = music_player_await(w->p, w->ticket, 5000);
   music_player_control_unlock(w->p);
   return NULL;
}

/* Wait (up to 5 s) until the tool holds the lock and has requested its track:
 * an open of the path alone could be an earlier test's straggler */
static bool wait_requested(waiter_t *w) {
   for (int i = 0; i < 5000 && !atomic_load(&w->requested); i++) {
      usleep(1000);
   }
   return atomic_load(&w->requested);
}

/* A preempting control ends a waiting request's wait; its open carries on */
static void test_preempt_ends_wait(void) {
   music_player_t *p = new_player(0);
   s_out.player = p;
   queue_add(music_player_queue(p), "plex:/library/parts/slow/1.flac");

   waiter_t w = { .p = p, .result = MUSIC_OPEN_PLAYING };
   pthread_t t;
   TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, tool_request, &w));
   bool opened = wait_requested(&w) && wait_opened("plex:/library/parts/slow/1.flac");
   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   lock_control(p, true);
   pthread_join(t, NULL);
   clock_gettime(CLOCK_MONOTONIC, &t1);
   TEST_ASSERT_TRUE(opened);
   TEST_ASSERT_EQUAL_INT(MUSIC_OPEN_PREEMPTED, w.result);
   /* Ended by the preempt, not by the await's own 5 s timeout */
   long waited_ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
   TEST_ASSERT_TRUE(waited_ms < 2000);

   atomic_store(&s_release_slow, true);
   unlock_control(p);
   TEST_ASSERT_TRUE(wait_recs(REC_BEGIN, -1, 1));
   free_player(p);
}

/* A stop during a slow open cancels it, and its request reports cancelled */
static void test_stop_cancels_open(void) {
   music_player_t *p = new_player(0);
   s_out.player = p;
   queue_add(music_player_queue(p), "plex:/library/parts/slow/1.flac");

   uint32_t ticket = 0;
   lock_control(p, false);
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         music_player_play(p, "plex:/library/parts/slow/1.flac", 0, &ticket));
   TEST_ASSERT_TRUE(wait_opened("plex:/library/parts/slow/1.flac"));
   music_player_stop(p);
   TEST_ASSERT_EQUAL_INT(MUSIC_OPEN_CANCELLED, music_player_await(p, ticket, 1000));
   unlock_control(p);

   music_player_status_t st;
   music_player_snapshot(p, &st);
   TEST_ASSERT_FALSE(st.playing);
   TEST_ASSERT_EQUAL_INT(0, recs_matching(REC_BEGIN, -1));
   free_player(p);
}

/* Two players on one queue: a remove by one moves the other's place */
static void test_two_players_follow_edits(void) {
   music_player_t *a = new_player(7);
   music_player_t *b = new_player(7);
   s_out.player = a;
   s_out.speed = 1;
   user_music_queue_t *uq = music_player_queue(a);
   TEST_ASSERT_TRUE(uq == music_player_queue(b));
   queue_add(uq, s_track_a);
   queue_add(uq, s_track_b);
   queue_add(uq, s_track_long);

   TEST_ASSERT_EQUAL_INT(SUCCESS, play(a, s_track_long, 2, NULL));
   TEST_ASSERT_TRUE(wait_recs(REC_BEGIN, -1, 1));

   bool start_failed = true;
   lock_control(b, true);
   TEST_ASSERT_EQUAL_INT(SUCCESS, music_player_remove(b, 0, &start_failed));
   unlock_control(b);
   TEST_ASSERT_FALSE(start_failed);

   /* Stopped, a player reports the entry at its place, not a lookup of its
    * track, so this reads the place itself */
   lock_control(a, true);
   music_player_stop(a);
   unlock_control(a);
   music_queue_entry_t now;
   pthread_mutex_lock(&uq->queue_mutex);
   int index = music_player_current(a, &now);
   pthread_mutex_unlock(&uq->queue_mutex);
   TEST_ASSERT_EQUAL_INT(1, index);
   TEST_ASSERT_EQUAL_STRING(s_track_long, now.path);

   free_player(a);
   free_player(b);
}

/* Shutting a player down while its output's write is blocked returns: the
 * write sees the player interrupted */
static void test_shutdown_during_blocked_write(void) {
   music_player_t *p = new_player(0);
   s_out.player = p;
   queue_add(music_player_queue(p), s_track_long);
   atomic_store(&s_out.block_write, true);

   TEST_ASSERT_EQUAL_INT(SUCCESS, play(p, s_track_long, 0, NULL));
   for (int i = 0; i < 5000 && !atomic_load(&s_out.write_blocked); i++) {
      usleep(1000);
   }
   TEST_ASSERT_TRUE(atomic_load(&s_out.write_blocked));
   free_player(p); /* hangs here if the write never returns */
   TEST_ASSERT_EQUAL_INT(0, music_player_running());
}

/* A reference outlives the player's shutdown: the player stays readable and
 * refuses control until the last release frees it */
static void test_reference_outlives_shutdown(void) {
   music_player_t *p = new_player(0);
   s_out.player = p;
   queue_add(music_player_queue(p), s_track_a);
   music_player_ref(p); /* as a tool call holding the player */

   free_player(p); /* the output's teardown: shutdown and its release */
   TEST_ASSERT_FALSE(music_player_control_lock(p, false));
   music_player_status_t st;
   music_player_snapshot(p, &st);
   TEST_ASSERT_EQUAL_INT(1, st.queue_length);
   music_player_release(p); /* the last one frees it (ASan checks) */
}

/* A shutdown ends a tool's wait for its track as closed, not pending */
static void test_shutdown_cancels_waiting_request(void) {
   music_player_t *p = new_player(0);
   s_out.player = p;
   queue_add(music_player_queue(p), "plex:/library/parts/slow/1.flac");
   music_player_ref(p); /* the tool's own reference */

   waiter_t w = { .p = p, .result = MUSIC_OPEN_PLAYING };
   pthread_t t;
   TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, tool_request, &w));
   bool opened = wait_requested(&w) && wait_opened("plex:/library/parts/slow/1.flac");
   /* Not free_player(): that lets the slow open finish first, and the request
    * could then truthfully resolve as playing.  The shutdown cancels the open
    * itself and preempts the waiting tool. */
   s_live[live_slot(p)] = NULL;
   music_player_shutdown(p);
   music_player_release(p);
   pthread_join(t, NULL);
   TEST_ASSERT_TRUE(opened);
   TEST_ASSERT_EQUAL_INT(MUSIC_OPEN_CLOSED, w.result);
   music_player_release(p);
}

/* A control request's own state change reaches the output */
static void test_notify_state_reaches_output(void) {
   music_player_t *p = new_player(0);
   s_out.player = p;
   lock_control(p, false);
   music_player_notify_state(p);
   music_player_notify_error(p, MUSIC_ERROR_PLAYBACK, "test");
   unlock_control(p);
   TEST_ASSERT_EQUAL_INT(1, recs_matching(REC_EVENT, MUSIC_EVENT_STATE));
   TEST_ASSERT_EQUAL_INT(1, errors_of(MUSIC_ERROR_PLAYBACK));
   free_player(p);
}

int main(void) {
   pthread_mutex_init(&s_out.mutex, NULL);
   pthread_cond_init(&s_out.cond, NULL);
   audio_decoder_init();
   if (write_flac(s_track_a, 2, 44100, 13230) != SUCCESS ||
       write_flac(s_track_b, 2, 44100, 13230) != SUCCESS ||
       write_flac(s_track_mono, 1, 22050, 6615) != SUCCESS ||
       write_flac(s_track_long, 2, 48000, 144000) != SUCCESS) {
      return 1;
   }

   UNITY_BEGIN();
   RUN_TEST(test_play_to_end_of_queue);
   RUN_TEST(test_gapless_advance);
   RUN_TEST(test_mono_track);
   RUN_TEST(test_requested_track_fails);
   RUN_TEST(test_advance_skips_failed_track);
   RUN_TEST(test_output_refuses_requested_track);
   RUN_TEST(test_output_refuses_advanced_track);
   RUN_TEST(test_too_many_failures_stop);
   RUN_TEST(test_unreachable_stops_all_remote_queue);
   RUN_TEST(test_unreachable_skips_in_mixed_queue);
   RUN_TEST(test_remote_prefetch_opens_once);
   RUN_TEST(test_pause_and_resume);
   RUN_TEST(test_seek_flushes);
   RUN_TEST(test_preempt_ends_wait);
   RUN_TEST(test_stop_cancels_open);
   RUN_TEST(test_two_players_follow_edits);
   RUN_TEST(test_shutdown_during_blocked_write);
   RUN_TEST(test_reference_outlives_shutdown);
   RUN_TEST(test_shutdown_cancels_waiting_request);
   RUN_TEST(test_notify_state_reaches_output);
   int rc = UNITY_END();

   unlink(s_track_a);
   unlink(s_track_b);
   unlink(s_track_mono);
   unlink(s_track_long);
   audio_decoder_cleanup();
   return rc;
}
