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
 * The audio converter: every channel combination it takes (one or two in,
 * one or two out), with and without resampling.
 */

#include <stdlib.h>

#include "audio/audio_converter.h"
#include "config/dawn_config.h"
#include "unity.h"

#define FRAMES 960

const dawn_config_t *config_get(void) {
   return NULL; /* not read: every converter here names its output */
}

void setUp(void) {
}

void tearDown(void) {
}

/* Convert FRAMES frames of in_ch channels; *out_frames receives the count */
static int16_t *convert(unsigned in_rate,
                        unsigned in_ch,
                        const int16_t *in,
                        unsigned out_rate,
                        unsigned out_ch,
                        size_t *out_frames) {
   audio_converter_params_t p = { .sample_rate = in_rate, .channels = in_ch };
   audio_converter_t *c = audio_converter_create_ex(&p, out_rate, out_ch);
   TEST_ASSERT_NOT_NULL(c);
   size_t max = audio_converter_max_output_frames(c, FRAMES);
   int16_t *out = calloc(max * out_ch, sizeof(int16_t));
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_EQUAL_INT(0, audio_converter_process(c, in, FRAMES, out, max, out_frames));
   audio_converter_destroy(c);
   return out;
}

/* Stereo to mono averages the two sides */
static void test_stereo_to_mono(void) {
   int16_t in[FRAMES * 2];
   for (int i = 0; i < FRAMES; i++) {
      in[i * 2] = 8000;
      in[i * 2 + 1] = 2000;
   }
   size_t n = 0;
   int16_t *out = convert(48000, 2, in, 48000, 1, &n);
   TEST_ASSERT_EQUAL_size_t(FRAMES, n);
   TEST_ASSERT_INT_WITHIN(2, 5000, out[0]);
   TEST_ASSERT_INT_WITHIN(2, 5000, out[FRAMES - 1]);
   free(out);
}

/* Mono to stereo copies the sample to both sides */
static void test_mono_to_stereo(void) {
   int16_t in[FRAMES];
   for (int i = 0; i < FRAMES; i++) {
      in[i] = 3000;
   }
   size_t n = 0;
   int16_t *out = convert(48000, 1, in, 48000, 2, &n);
   TEST_ASSERT_EQUAL_size_t(FRAMES, n);
   TEST_ASSERT_INT_WITHIN(2, 3000, out[0]);
   TEST_ASSERT_INT_WITHIN(2, 3000, out[1]);
   free(out);
}

/* Mono resampled stays mono, about ratio times as many frames */
static void test_mono_resampled(void) {
   int16_t in[FRAMES] = { 0 };
   size_t n = 0;
   int16_t *out = convert(24000, 1, in, 48000, 1, &n);
   TEST_ASSERT_TRUE(n > FRAMES);
   free(out);
}

/* Stereo resampled down to mono keeps every input frame's worth of output
 * (the resampler's buffer is sized by input channels) */
static void test_stereo_resampled_to_mono(void) {
   int16_t in[FRAMES * 2];
   for (int i = 0; i < FRAMES * 2; i++) {
      in[i] = 4000;
   }
   audio_converter_params_t p = { .sample_rate = 24000, .channels = 2 };
   audio_converter_t *c = audio_converter_create_ex(&p, 48000, 1);
   TEST_ASSERT_NOT_NULL(c);
   size_t max = audio_converter_max_output_frames(c, FRAMES);
   int16_t *out = calloc(max, sizeof(int16_t));
   TEST_ASSERT_NOT_NULL(out);
   size_t total = 0;
   for (int round = 0; round < 10; round++) {
      size_t n = 0;
      TEST_ASSERT_EQUAL_INT(0, audio_converter_process(c, in, FRAMES, out, max, &n));
      total += n;
   }
   /* 10 x 960 frames at 2x: ~19200 frames, less the resampler's latency */
   TEST_ASSERT_TRUE(total > 18000);
   TEST_ASSERT_INT_WITHIN(200, 4000, out[max / 4]);
   free(out);
   audio_converter_destroy(c);
}

/* More than two channels is refused at creation */
static void test_three_channels_refused(void) {
   audio_converter_params_t p = { .sample_rate = 48000, .channels = 3 };
   TEST_ASSERT_NULL(audio_converter_create_ex(&p, 48000, 2));
   p.channels = 2;
   TEST_ASSERT_NULL(audio_converter_create_ex(&p, 48000, 3));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_stereo_to_mono);
   RUN_TEST(test_mono_to_stereo);
   RUN_TEST(test_mono_resampled);
   RUN_TEST(test_stereo_resampled_to_mono);
   RUN_TEST(test_three_channels_refused);
   return UNITY_END();
}
