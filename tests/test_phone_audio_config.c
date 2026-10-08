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
 * Round-trip (write -> parse) symmetry test for the [phone] call-audio config.
 * This is the CI gate for the "saves but doesn't load" clobber class: if a key
 * is added to the parser but not the writer (or vice versa), the round-trip drops
 * or diverges on that field and this test fails.
 */

#include <stdio.h>
#include <string.h>

#include "toml.h"
#include "tools/phone_audio_config.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* Serialize @in as a [phone] section, parse it back, return the result. */
static phone_audio_config_t round_trip(const phone_audio_config_t *in) {
   char buf[1024];
   memset(buf, 0, sizeof(buf));

   FILE *f = fmemopen(buf, sizeof(buf), "w");
   TEST_ASSERT_NOT_NULL(f);
   fputs("[phone]\n", f);
   phone_audio_config_write(f, in);
   fclose(f); /* flushes + NUL-terminates within buf */

   char errbuf[128] = { 0 };
   toml_table_t *root = toml_parse(buf, errbuf, sizeof(errbuf));
   TEST_ASSERT_NOT_NULL_MESSAGE(root, errbuf);
   toml_table_t *phone = toml_table_in(root, "phone");
   TEST_ASSERT_NOT_NULL(phone);

   phone_audio_config_t out = phone_audio_config_default();
   phone_audio_config_parse(phone, &out);
   toml_free(root);
   return out;
}

static void assert_apm_equal(const phone_apm_config_t *a, const phone_apm_config_t *b) {
   TEST_ASSERT_EQUAL(a->agc_enabled, b->agc_enabled);
   TEST_ASSERT_FLOAT_WITHIN(0.0001f, a->fixed_gain_db, b->fixed_gain_db);
   TEST_ASSERT_FLOAT_WITHIN(0.0001f, a->max_gain_change_db_per_s, b->max_gain_change_db_per_s);
   TEST_ASSERT_FLOAT_WITHIN(0.0001f, a->max_output_noise_dbfs, b->max_output_noise_dbfs);
   TEST_ASSERT_EQUAL(a->ns_level, b->ns_level);
   TEST_ASSERT_EQUAL(a->high_pass, b->high_pass);
}

static void assert_equal(const phone_audio_config_t *a, const phone_audio_config_t *b) {
   TEST_ASSERT_EQUAL_STRING(a->pcm_port, b->pcm_port);
   assert_apm_equal(&a->uplink, &b->uplink);
   TEST_ASSERT_EQUAL(a->uplink.echo_cancel, b->uplink.echo_cancel); /* uplink-only key */
   assert_apm_equal(&a->downlink, &b->downlink);
   TEST_ASSERT_EQUAL(a->downlink_use_apm, b->downlink_use_apm);
   TEST_ASSERT_FLOAT_WITHIN(0.0001f, a->downlink_gain, b->downlink_gain);
}

/* Defaults survive a round-trip unchanged. */
static void test_defaults_round_trip(void) {
   phone_audio_config_t in = phone_audio_config_default();
   phone_audio_config_t out = round_trip(&in);
   assert_equal(&in, &out);
}

/* Every field set to a NON-default value round-trips — the real clobber guard:
 * a parsed-but-unwritten key would revert to its default here. */
static void test_nondefault_round_trip(void) {
   phone_audio_config_t in = phone_audio_config_default();
   snprintf(in.pcm_port, sizeof(in.pcm_port), "%s", "/dev/ttyUSB4");
   in.uplink.agc_enabled = false;             /* default true */
   in.uplink.fixed_gain_db = 12.5f;           /* default 1.0 */
   in.uplink.max_gain_change_db_per_s = 1.5f; /* default 3.0 */
   in.uplink.max_output_noise_dbfs = -40.0f;  /* default -50.0 */
   in.uplink.ns_level = PHONE_NS_HIGH;        /* default moderate */
   in.uplink.high_pass = false;               /* default true */
   in.uplink.echo_cancel = false;             /* default true */
   /* Downlink: flip every round-tripped field off its default. */
   in.downlink.agc_enabled = false;             /* default true */
   in.downlink.fixed_gain_db = 3.0f;            /* default 6.0 */
   in.downlink.max_gain_change_db_per_s = 6.0f; /* default 3.0 */
   in.downlink.max_output_noise_dbfs = -30.0f;  /* default -50.0 */
   in.downlink.ns_level = PHONE_NS_LOW;         /* default off */
   in.downlink.high_pass = false;               /* default true */
   in.downlink_use_apm = false;                 /* default true */
   in.downlink_gain = 9.5f;                     /* default 0 */

   phone_audio_config_t out = round_trip(&in);
   assert_equal(&in, &out);
}

/* A full-length /dev/serial/by-id alias (93 chars) round-trips — pins the
 * PHONE_PCM_PORT_MAX buffer growth (a 64-byte buffer would truncate it). */
static void test_by_id_pcm_port_round_trip(void) {
   phone_audio_config_t in = phone_audio_config_default();
   const char *by_id =
       "/dev/serial/by-id/"
       "usb-SimTech__Incorporated_SimTech__Incorporated_0123456789ABCDEF-if06-port0";
   TEST_ASSERT_LESS_THAN_UINT(sizeof(in.pcm_port), strlen(by_id) + 1); /* must fit */
   snprintf(in.pcm_port, sizeof(in.pcm_port), "%s", by_id);
   phone_audio_config_t out = round_trip(&in);
   TEST_ASSERT_EQUAL_STRING(by_id, out.pcm_port);
}

/* phone_pcm_port_validate: pure syntax accept/reject (no device needed). */
static void test_pcm_port_validate_accept(void) {
   TEST_ASSERT_TRUE(phone_pcm_port_validate("/dev/ttyUSB0"));
   TEST_ASSERT_TRUE(phone_pcm_port_validate("/dev/ttyUSB100"));
   TEST_ASSERT_TRUE(phone_pcm_port_validate("/dev/ttyACM2"));
   TEST_ASSERT_TRUE(phone_pcm_port_validate(
       "/dev/serial/by-id/usb-SimTech__Incorporated_0123456789ABCDEF-if06-port0"));
   TEST_ASSERT_TRUE(phone_pcm_port_validate("/dev/serial/by-path/platform-xhci-usb-0:2.1:1.5"));
}

static void test_pcm_port_validate_reject(void) {
   TEST_ASSERT_FALSE(phone_pcm_port_validate(NULL));
   TEST_ASSERT_FALSE(phone_pcm_port_validate(""));
   TEST_ASSERT_FALSE(phone_pcm_port_validate("/dev/ttyUSB"));           /* no digits */
   TEST_ASSERT_FALSE(phone_pcm_port_validate("/dev/ttyUSBfoo"));        /* non-digit */
   TEST_ASSERT_FALSE(phone_pcm_port_validate("/dev/ttyUSB1234"));       /* > 3 digits */
   TEST_ASSERT_FALSE(phone_pcm_port_validate("/etc/passwd"));           /* wrong prefix */
   TEST_ASSERT_FALSE(phone_pcm_port_validate("/dev/serial/by-id/"));    /* empty name */
   TEST_ASSERT_FALSE(phone_pcm_port_validate("/dev/serial/by-id/a/b")); /* nested */
   TEST_ASSERT_FALSE(phone_pcm_port_validate("/dev/serial/by-id/../../etc/passwd"));
}

/* Each NS enum value survives the string<->enum mapping. */
static void test_ns_level_round_trip(void) {
   const phone_ns_level_t levels[] = { PHONE_NS_OFF, PHONE_NS_LOW, PHONE_NS_MODERATE, PHONE_NS_HIGH,
                                       PHONE_NS_VERYHIGH };
   for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
      phone_audio_config_t in = phone_audio_config_default();
      in.uplink.ns_level = levels[i];
      phone_audio_config_t out = round_trip(&in);
      TEST_ASSERT_EQUAL(levels[i], out.uplink.ns_level);
   }
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_defaults_round_trip);
   RUN_TEST(test_nondefault_round_trip);
   RUN_TEST(test_by_id_pcm_port_round_trip);
   RUN_TEST(test_pcm_port_validate_accept);
   RUN_TEST(test_pcm_port_validate_reject);
   RUN_TEST(test_ns_level_round_trip);
   return UNITY_END();
}
