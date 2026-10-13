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
 * mono or stereo only, so a 5.1 FLAC or Ogg Vorbis file is downmixed into a
 * stereo-sized buffer.  (FLAC used to refuse it and play a silently skipped
 * track; Ogg wrote all six channels and overran the caller's buffer.)
 */

#include <FLAC/stream_encoder.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef DAWN_ENABLE_OGG
#include <vorbis/vorbisenc.h>
#endif

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

#ifdef DAWN_ENABLE_OGG
/* Write all pending pages of an Ogg stream to fp */
static void write_pages(ogg_stream_state *os, FILE *fp, bool flush) {
   ogg_page og;
   while (flush ? ogg_stream_flush(os, &og) : ogg_stream_pageout(os, &og)) {
      fwrite(og.header, 1, (size_t)og.header_len, fp);
      fwrite(og.body, 1, (size_t)og.body_len, fp);
   }
}

/* Write a Vorbis file with a 440 Hz sine on one channel, silence elsewhere */
static void write_vorbis(int channels, int tone_channel) {
   snprintf(s_path, sizeof(s_path), "/tmp/dawn_test_chXXXXXX.ogg");
   int fd = mkstemps(s_path, 4);
   TEST_ASSERT_TRUE(fd >= 0);
   FILE *fp = fdopen(fd, "wb");
   TEST_ASSERT_NOT_NULL(fp);

   vorbis_info vi;
   vorbis_comment vc;
   vorbis_dsp_state vd;
   vorbis_block vb;
   ogg_stream_state os;
   vorbis_info_init(&vi);
   TEST_ASSERT_EQUAL_INT(0, vorbis_encode_init_vbr(&vi, channels, RATE, 0.5f));
   vorbis_comment_init(&vc);
   vorbis_analysis_init(&vd, &vi);
   vorbis_block_init(&vd, &vb);
   ogg_stream_init(&os, 1);

   ogg_packet header, header_comm, header_code;
   vorbis_analysis_headerout(&vd, &vc, &header, &header_comm, &header_code);
   ogg_stream_packetin(&os, &header);
   ogg_stream_packetin(&os, &header_comm);
   ogg_stream_packetin(&os, &header_code);
   write_pages(&os, fp, true);

   float **buf = vorbis_analysis_buffer(&vd, FRAMES);
   for (int ch = 0; ch < channels; ch++) {
      for (int i = 0; i < FRAMES; i++) {
         buf[ch][i] = ch == tone_channel ? 0.5f * sinf(2.0f * (float)M_PI * 440.0f * i / RATE)
                                         : 0.0f;
      }
   }
   vorbis_analysis_wrote(&vd, FRAMES);
   vorbis_analysis_wrote(&vd, 0); /* end of stream */

   ogg_packet op;
   while (vorbis_analysis_blockout(&vd, &vb) == 1) {
      vorbis_analysis(&vb, NULL);
      vorbis_bitrate_addblock(&vb);
      while (vorbis_bitrate_flushpacket(&vd, &op)) {
         ogg_stream_packetin(&os, &op);
         write_pages(&os, fp, false);
      }
   }
   write_pages(&os, fp, true);

   ogg_stream_clear(&os);
   vorbis_block_clear(&vb);
   vorbis_dsp_clear(&vd);
   vorbis_comment_clear(&vc);
   vorbis_info_clear(&vi);
   fclose(fp);
}

static void test_51_ogg_reads_as_stereo(void) {
   write_vorbis(6, 0); /* sine on front left (Vorbis order: L C R ...) */
   audio_decoder_t *dec = audio_decoder_open(s_path);
   TEST_ASSERT_NOT_NULL(dec);

   audio_decoder_info_t info;
   TEST_ASSERT_EQUAL_INT(AUDIO_DECODER_SUCCESS, audio_decoder_get_info(dec, &info));
   TEST_ASSERT_EQUAL_UINT8(2, info.channels);

   /* Stereo-sized buffer with a canary after it: six channels written here
    * is the overrun this test guards against */
   int16_t buf[READ_FRAMES * 2 + 16];
   for (int i = READ_FRAMES * 2; i < READ_FRAMES * 2 + 16; i++) {
      buf[i] = CANARY;
   }
   double sum_l = 0.0;
   double sum_r = 0.0;
   long total = 0;
   ssize_t n;
   while ((n = audio_decoder_read(dec, buf, READ_FRAMES)) > 0) {
      for (ssize_t i = 0; i < n; i++) {
         sum_l += (double)buf[i * 2] * buf[i * 2];
         sum_r += (double)buf[i * 2 + 1] * buf[i * 2 + 1];
      }
      total += n;
   }
   TEST_ASSERT_EQUAL_INT(0, n);
   for (int i = READ_FRAMES * 2; i < READ_FRAMES * 2 + 16; i++) {
      TEST_ASSERT_EQUAL_HEX16(CANARY, (uint16_t)buf[i]);
   }
   TEST_ASSERT_TRUE(total >= FRAMES / 2);

   /* Front left stays left (lossy, so compare energy, not samples) */
   double rms_l = sqrt(sum_l / (double)total);
   double rms_r = sqrt(sum_r / (double)total);
   TEST_ASSERT_TRUE(rms_l > 1000.0);
   TEST_ASSERT_TRUE(rms_r < rms_l / 20.0);
   audio_decoder_close(dec);
}
#endif

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_51_flac_reads_as_stereo);
   RUN_TEST(test_51_flac_drops_lfe);
   RUN_TEST(test_stereo_flac_is_unchanged);
#ifdef DAWN_ENABLE_OGG
   RUN_TEST(test_51_ogg_reads_as_stereo);
#endif
   return UNITY_END();
}
