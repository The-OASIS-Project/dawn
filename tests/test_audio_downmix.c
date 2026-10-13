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
 * Unit tests for audio_downmix.c: channel layouts per format, BS.775 weights,
 * and no clipping at full scale.
 */

#include <string.h>

#include "audio_downmix.h"
#include "dawn_error.h"
#include "unity.h"

#define FRAMES 4

/* 5.1 front-left weight after normalization: 1 / (1 + 0.707 + 0.707) */
#define FIVE_ONE_FRONT (32767.0f / (1.0f + 2.0f * 0.70710678f))

static float s_planes[8][FRAMES];
static const float *s_plane_ptrs[8];

void setUp(void) {
   memset(s_planes, 0, sizeof(s_planes));
   for (int ch = 0; ch < 8; ch++) {
      s_plane_ptrs[ch] = s_planes[ch];
   }
}

void tearDown(void) {
}

static void fill(int ch, float v) {
   for (int i = 0; i < FRAMES; i++) {
      s_planes[ch][i] = v;
   }
}

static void test_init_rejects_unsupported_counts(void) {
   audio_downmix_t dm;
   TEST_ASSERT_EQUAL_INT(FAILURE, audio_downmix_init(&dm, 2, AUDIO_CHANNEL_ORDER_FLAC));
   TEST_ASSERT_EQUAL_INT(FAILURE, audio_downmix_init(&dm, 9, AUDIO_CHANNEL_ORDER_FLAC));
   TEST_ASSERT_EQUAL_INT(FAILURE, audio_downmix_init(NULL, 6, AUDIO_CHANNEL_ORDER_FLAC));
   for (int ch = 3; ch <= 8; ch++) {
      TEST_ASSERT_EQUAL_INT(SUCCESS, audio_downmix_init(&dm, ch, AUDIO_CHANNEL_ORDER_FLAC));
      TEST_ASSERT_EQUAL_INT(SUCCESS, audio_downmix_init(&dm, ch, AUDIO_CHANNEL_ORDER_VORBIS));
   }
}

static void test_flac_51_front_left_stays_left(void) {
   audio_downmix_t dm;
   int16_t out[FRAMES * 2];
   audio_downmix_init(&dm, 6, AUDIO_CHANNEL_ORDER_FLAC);
   fill(0, 1.0f); /* FL */
   audio_downmix_float(&dm, s_plane_ptrs, FRAMES, out);
   TEST_ASSERT_INT_WITHIN(2, (int)FIVE_ONE_FRONT, out[0]);
   TEST_ASSERT_EQUAL_INT16(0, out[1]);
}

static void test_flac_51_centre_is_index_2(void) {
   audio_downmix_t dm;
   int16_t out[FRAMES * 2];
   audio_downmix_init(&dm, 6, AUDIO_CHANNEL_ORDER_FLAC);
   fill(2, 1.0f); /* C */
   audio_downmix_float(&dm, s_plane_ptrs, FRAMES, out);
   TEST_ASSERT_INT_WITHIN(2, (int)(FIVE_ONE_FRONT * 0.70710678f), out[0]);
   TEST_ASSERT_EQUAL_INT16(out[0], out[1]);
}

static void test_vorbis_51_centre_is_index_1(void) {
   audio_downmix_t dm;
   int16_t out[FRAMES * 2];
   audio_downmix_init(&dm, 6, AUDIO_CHANNEL_ORDER_VORBIS);
   fill(1, 1.0f); /* C in Vorbis order; FR in FLAC order */
   audio_downmix_float(&dm, s_plane_ptrs, FRAMES, out);
   TEST_ASSERT_INT_WITHIN(2, (int)(FIVE_ONE_FRONT * 0.70710678f), out[0]);
   TEST_ASSERT_EQUAL_INT16(out[0], out[1]);
}

static void test_lfe_is_dropped(void) {
   audio_downmix_t dm;
   int16_t out[FRAMES * 2];
   audio_downmix_init(&dm, 6, AUDIO_CHANNEL_ORDER_FLAC);
   fill(3, 1.0f); /* LFE in FLAC order */
   audio_downmix_float(&dm, s_plane_ptrs, FRAMES, out);
   TEST_ASSERT_EQUAL_INT16(0, out[0]);
   TEST_ASSERT_EQUAL_INT16(0, out[1]);

   audio_downmix_init(&dm, 6, AUDIO_CHANNEL_ORDER_VORBIS);
   setUp();
   fill(5, 1.0f); /* LFE in Vorbis order */
   audio_downmix_float(&dm, s_plane_ptrs, FRAMES, out);
   TEST_ASSERT_EQUAL_INT16(0, out[0]);
   TEST_ASSERT_EQUAL_INT16(0, out[1]);
}

static void test_full_scale_does_not_clip(void) {
   audio_downmix_t dm;
   int16_t out[FRAMES * 2];
   audio_downmix_init(&dm, 8, AUDIO_CHANNEL_ORDER_FLAC);
   for (int ch = 0; ch < 8; ch++) {
      fill(ch, 1.0f);
   }
   audio_downmix_float(&dm, s_plane_ptrs, FRAMES, out);
   /* Normalized weights sum to 1: the output reaches full scale, no further */
   TEST_ASSERT_INT_WITHIN(2, 32767, out[0]);
   TEST_ASSERT_INT_WITHIN(2, 32767, out[1]);

   for (int ch = 0; ch < 8; ch++) {
      fill(ch, -1.0f);
   }
   audio_downmix_float(&dm, s_plane_ptrs, FRAMES, out);
   TEST_ASSERT_INT_WITHIN(2, -32767, out[0]);
}

static void test_int32_24bit_scales_to_16bit(void) {
   audio_downmix_t dm;
   int16_t out[FRAMES * 2];
   int32_t planes[6][FRAMES];
   const int32_t *ptrs[6];
   memset(planes, 0, sizeof(planes));
   for (int ch = 0; ch < 6; ch++) {
      ptrs[ch] = planes[ch];
   }
   for (int i = 0; i < FRAMES; i++) {
      planes[1][i] = (1 << 23) - 1; /* FR, 24-bit full scale */
   }
   audio_downmix_init(&dm, 6, AUDIO_CHANNEL_ORDER_FLAC);
   audio_downmix_int32(&dm, ptrs, 24, FRAMES, out);
   TEST_ASSERT_EQUAL_INT16(0, out[0]);
   TEST_ASSERT_INT_WITHIN(2, (int)FIVE_ONE_FRONT, out[1]);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_init_rejects_unsupported_counts);
   RUN_TEST(test_flac_51_front_left_stays_left);
   RUN_TEST(test_flac_51_centre_is_index_2);
   RUN_TEST(test_vorbis_51_centre_is_index_1);
   RUN_TEST(test_lfe_is_dropped);
   RUN_TEST(test_full_scale_does_not_clip);
   RUN_TEST(test_int32_24bit_scales_to_16bit);
   return UNITY_END();
}
