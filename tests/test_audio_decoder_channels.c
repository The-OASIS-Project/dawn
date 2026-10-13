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
 * Unit tests for the decoder channel contract: audio_decoder_read() returns
 * mono or stereo only, so a 5.1 FLAC is downmixed into a stereo-sized buffer
 * (it used to be refused, and played as a silently skipped track).
 */

#include <FLAC/stream_encoder.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio/audio_decoder.h"
#include "unity.h"

#define RATE 48000
#define FRAMES 4800 /* 100 ms */
#define TONE 8000   /* sample value on the channel that carries the tone */
#define READ_FRAMES 960
#define CANARY 0x5A5A

static char s_path[64];

void setUp(void) {
   audio_decoder_init();
   s_path[0] = '\0';
}

void tearDown(void) {
   if (s_path[0]) {
      unlink(s_path);
   }
   audio_decoder_cleanup();
}

/* Write a 16-bit FLAC with a constant value on one channel, zero elsewhere */
static void write_flac(unsigned channels, unsigned tone_channel) {
   snprintf(s_path, sizeof(s_path), "/tmp/dawn_test_chXXXXXX.flac");
   int fd = mkstemps(s_path, 5);
   TEST_ASSERT_TRUE(fd >= 0);
   close(fd);

   FLAC__StreamEncoder *enc = FLAC__stream_encoder_new();
   TEST_ASSERT_NOT_NULL(enc);
   FLAC__stream_encoder_set_channels(enc, channels);
   FLAC__stream_encoder_set_bits_per_sample(enc, 16);
   FLAC__stream_encoder_set_sample_rate(enc, RATE);
   FLAC__stream_encoder_set_total_samples_estimate(enc, FRAMES);
   TEST_ASSERT_EQUAL_INT(FLAC__STREAM_ENCODER_INIT_STATUS_OK,
                         FLAC__stream_encoder_init_file(enc, s_path, NULL, NULL));

   FLAC__int32 *pcm = calloc((size_t)FRAMES * channels, sizeof(*pcm));
   TEST_ASSERT_NOT_NULL(pcm);
   for (unsigned i = 0; i < FRAMES; i++) {
      pcm[i * channels + tone_channel] = TONE;
   }
   TEST_ASSERT_TRUE(FLAC__stream_encoder_process_interleaved(enc, pcm, FRAMES));
   TEST_ASSERT_TRUE(FLAC__stream_encoder_finish(enc));
   FLAC__stream_encoder_delete(enc);
   free(pcm);
}

/* Read the whole file into a stereo-sized buffer with a canary after it.
 * Returns the frame count; *left / *right get the last frame's samples. */
static long read_all(audio_decoder_t *dec, int16_t *left, int16_t *right) {
   int16_t buf[READ_FRAMES * 2 + 16];
   for (int i = READ_FRAMES * 2; i < READ_FRAMES * 2 + 16; i++) {
      buf[i] = CANARY;
   }
   long total = 0;
   ssize_t n;
   while ((n = audio_decoder_read(dec, buf, READ_FRAMES)) > 0) {
      *left = buf[(n - 1) * 2];
      *right = buf[(n - 1) * 2 + 1];
      total += n;
   }
   TEST_ASSERT_EQUAL_INT(0, n);
   for (int i = READ_FRAMES * 2; i < READ_FRAMES * 2 + 16; i++) {
      TEST_ASSERT_EQUAL_HEX16(CANARY, (uint16_t)buf[i]);
   }
   return total;
}

static void test_51_flac_reads_as_stereo(void) {
   write_flac(6, 0); /* tone on front left */
   audio_decoder_t *dec = audio_decoder_open(s_path);
   TEST_ASSERT_NOT_NULL(dec);

   audio_decoder_info_t info;
   TEST_ASSERT_EQUAL_INT(AUDIO_DECODER_SUCCESS, audio_decoder_get_info(dec, &info));
   TEST_ASSERT_EQUAL_UINT8(2, info.channels);

   int16_t left = 0;
   int16_t right = 0;
   TEST_ASSERT_EQUAL_INT(FRAMES, read_all(dec, &left, &right));
   /* Front left stays left, at 1 / (1 + 2 * 0.707) after normalization */
   TEST_ASSERT_INT_WITHIN(4, (int)(TONE / (1.0 + 2.0 * 0.70710678)), left);
   TEST_ASSERT_EQUAL_INT16(0, right);
   audio_decoder_close(dec);
}

static void test_51_flac_drops_lfe(void) {
   write_flac(6, 3); /* tone on the LFE channel */
   audio_decoder_t *dec = audio_decoder_open(s_path);
   TEST_ASSERT_NOT_NULL(dec);
   int16_t left = 1;
   int16_t right = 1;
   TEST_ASSERT_EQUAL_INT(FRAMES, read_all(dec, &left, &right));
   TEST_ASSERT_EQUAL_INT16(0, left);
   TEST_ASSERT_EQUAL_INT16(0, right);
   audio_decoder_close(dec);
}

static void test_stereo_flac_is_unchanged(void) {
   write_flac(2, 1); /* tone on the right */
   audio_decoder_t *dec = audio_decoder_open(s_path);
   TEST_ASSERT_NOT_NULL(dec);
   audio_decoder_info_t info;
   audio_decoder_get_info(dec, &info);
   TEST_ASSERT_EQUAL_UINT8(2, info.channels);
   int16_t left = 1;
   int16_t right = 0;
   TEST_ASSERT_EQUAL_INT(FRAMES, read_all(dec, &left, &right));
   TEST_ASSERT_EQUAL_INT16(0, left);
   TEST_ASSERT_EQUAL_INT16(TONE, right);
   audio_decoder_close(dec);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_51_flac_reads_as_stereo);
   RUN_TEST(test_51_flac_drops_lfe);
   RUN_TEST(test_stereo_flac_is_unchanged);
   return UNITY_END();
}
