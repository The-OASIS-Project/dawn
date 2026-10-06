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
 * Unit tests for crypto_store encrypt/decrypt (libsodium crypto_secretbox).
 */

#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/crypto_store.h"
#include "llm/llm_key_tag.h"
#include "unity.h"

static char s_tmpdir[256];
static bool s_tmpdir_created = false;

void setUp(void) {
   if (!s_tmpdir_created) {
      snprintf(s_tmpdir, sizeof(s_tmpdir), "/tmp/dawn_crypto_test_XXXXXX");
      TEST_ASSERT_NOT_NULL(mkdtemp(s_tmpdir));
      extern dawn_config_t g_config;
      strncpy(g_config.paths.data_dir, s_tmpdir, sizeof(g_config.paths.data_dir) - 1);
      s_tmpdir_created = true;
   }
}

void tearDown(void) {
}

/* ── Init ────────────────────────────────────────────────────────────────── */

static void test_init_succeeds(void) {
   TEST_ASSERT_EQUAL_INT(0, crypto_store_init());
}

static void test_ready_after_init(void) {
   crypto_store_init();
   TEST_ASSERT_TRUE(crypto_store_ready());
}

/* ── Encrypt / Decrypt Roundtrip ─────────────────────────────────────────── */

static void test_roundtrip_string(void) {
   const char *plaintext = "Hello, World!";
   size_t pt_len = strlen(plaintext);
   size_t needed = crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES + pt_len;

   unsigned char ct[256];
   size_t ct_written = 0;
   TEST_ASSERT_EQUAL_INT(0, crypto_store_encrypt(plaintext, pt_len, ct, sizeof(ct), &ct_written));
   TEST_ASSERT_EQUAL_size_t(needed, ct_written);

   char decrypted[256];
   size_t pt_written = 0;
   TEST_ASSERT_EQUAL_INT(0, crypto_store_decrypt(ct, ct_written, decrypted, sizeof(decrypted),
                                                 &pt_written));
   TEST_ASSERT_EQUAL_size_t(pt_len, pt_written);
   TEST_ASSERT_EQUAL_MEMORY(plaintext, decrypted, pt_len);
}

static void test_roundtrip_binary(void) {
   unsigned char binary[] = { 0x00, 0x01, 0xFF, 0x80, 0x00, 0x7F, 0xFE, 0x00 };
   size_t pt_len = sizeof(binary);

   unsigned char ct[256];
   size_t ct_written = 0;
   TEST_ASSERT_EQUAL_INT(0, crypto_store_encrypt(binary, pt_len, ct, sizeof(ct), &ct_written));

   unsigned char decrypted[256];
   size_t pt_written = 0;
   TEST_ASSERT_EQUAL_INT(0, crypto_store_decrypt(ct, ct_written, decrypted, sizeof(decrypted),
                                                 &pt_written));
   TEST_ASSERT_EQUAL_size_t(pt_len, pt_written);
   TEST_ASSERT_EQUAL_MEMORY(binary, decrypted, pt_len);
}

static void test_roundtrip_empty(void) {
   const char *plaintext = "";
   size_t pt_len = 0;

   unsigned char ct[256];
   size_t ct_written = 0;
   TEST_ASSERT_EQUAL_INT(0, crypto_store_encrypt(plaintext, pt_len, ct, sizeof(ct), &ct_written));
   TEST_ASSERT_EQUAL_size_t(crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES, ct_written);

   char decrypted[256];
   size_t pt_written = 0;
   TEST_ASSERT_EQUAL_INT(0, crypto_store_decrypt(ct, ct_written, decrypted, sizeof(decrypted),
                                                 &pt_written));
   TEST_ASSERT_EQUAL_size_t(0, pt_written);
}

/* ── out_written ─────────────────────────────────────────────────────────── */

static void test_encrypt_out_written(void) {
   const char *plaintext = "test data";
   size_t pt_len = strlen(plaintext);
   size_t expected = crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES + pt_len;

   unsigned char ct[256];
   size_t ct_written = 0;
   TEST_ASSERT_EQUAL_INT(0, crypto_store_encrypt(plaintext, pt_len, ct, sizeof(ct), &ct_written));
   TEST_ASSERT_EQUAL_size_t(expected, ct_written);
}

static void test_encrypt_out_written_null(void) {
   const char *plaintext = "test";
   unsigned char ct[256];
   TEST_ASSERT_EQUAL_INT(0,
                         crypto_store_encrypt(plaintext, strlen(plaintext), ct, sizeof(ct), NULL));
}

/* ── Buffer Too Small ────────────────────────────────────────────────────── */

static void test_encrypt_buffer_too_small(void) {
   const char *plaintext = "Hello, World!";
   unsigned char ct[4];
   size_t ct_written = 0;
   TEST_ASSERT_EQUAL_INT(1, crypto_store_encrypt(plaintext, strlen(plaintext), ct, sizeof(ct),
                                                 &ct_written));
}

static void test_decrypt_buffer_too_small(void) {
   const char *plaintext = "Hello, World!";
   size_t pt_len = strlen(plaintext);

   unsigned char ct[256];
   size_t ct_written = 0;
   crypto_store_encrypt(plaintext, pt_len, ct, sizeof(ct), &ct_written);

   char decrypted[2];
   size_t pt_written = 0;
   TEST_ASSERT_EQUAL_INT(1, crypto_store_decrypt(ct, ct_written, decrypted, sizeof(decrypted),
                                                 &pt_written));
}

/* ── Corrupt / Invalid Data ──────────────────────────────────────────────── */

static void test_decrypt_corrupt_data(void) {
   const char *plaintext = "secret message";
   size_t pt_len = strlen(plaintext);

   unsigned char ct[256];
   size_t ct_written = 0;
   crypto_store_encrypt(plaintext, pt_len, ct, sizeof(ct), &ct_written);

   /* Flip a byte in the ciphertext body (after the nonce) */
   ct[crypto_secretbox_NONCEBYTES + 2] ^= 0xFF;

   char decrypted[256];
   size_t pt_written = 0;
   TEST_ASSERT_EQUAL_INT(1, crypto_store_decrypt(ct, ct_written, decrypted, sizeof(decrypted),
                                                 &pt_written));
}

static void test_decrypt_truncated(void) {
   unsigned char short_ct[8] = { 0 };
   char decrypted[256];
   size_t pt_written = 0;
   TEST_ASSERT_EQUAL_INT(1, crypto_store_decrypt(short_ct, sizeof(short_ct), decrypted,
                                                 sizeof(decrypted), &pt_written));
}

/* ── NULL Inputs ─────────────────────────────────────────────────────────── */

static void test_encrypt_null_input(void) {
   unsigned char ct[256];
   TEST_ASSERT_EQUAL_INT(1, crypto_store_encrypt(NULL, 10, ct, sizeof(ct), NULL));
}

static void test_encrypt_null_output(void) {
   const char *plaintext = "test";
   TEST_ASSERT_EQUAL_INT(1, crypto_store_encrypt(plaintext, strlen(plaintext), NULL, 0, NULL));
}

static void test_decrypt_null_input(void) {
   char decrypted[256];
   TEST_ASSERT_EQUAL_INT(1, crypto_store_decrypt(NULL, 10, decrypted, sizeof(decrypted), NULL));
}

static void test_decrypt_null_output(void) {
   unsigned char ct[64] = { 0 };
   TEST_ASSERT_EQUAL_INT(1, crypto_store_decrypt(ct, sizeof(ct), NULL, 0, NULL));
}

/* ── Shutdown ────────────────────────────────────────────────────────────── */

static void test_shutdown_clears_ready(void) {
   crypto_store_init();
   TEST_ASSERT_TRUE(crypto_store_ready());
   crypto_store_shutdown();
   TEST_ASSERT_FALSE(crypto_store_ready());
}

/* ── Main ────────────────────────────────────────────────────────────────── */

/* ── Keyed digest / key tags ─────────────────────────────────────────────── */

static void test_keyed_digest(void) {
   unsigned char a[16], b[16], c[16], d[16];
   TEST_ASSERT_EQUAL_INT(0, crypto_store_keyed_digest("test-ctx", "k1", 2, a, sizeof(a)));
   TEST_ASSERT_EQUAL_INT(0, crypto_store_keyed_digest("test-ctx", "k1", 2, b, sizeof(b)));
   TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof(a));
   TEST_ASSERT_EQUAL_INT(0, crypto_store_keyed_digest("test-ctx", "k2", 2, c, sizeof(c)));
   TEST_ASSERT_NOT_EQUAL(0, memcmp(a, c, sizeof(a)));
   /* Another purpose, another digest of the same input. */
   TEST_ASSERT_EQUAL_INT(0, crypto_store_keyed_digest("other-cx", "k1", 2, d, sizeof(d)));
   TEST_ASSERT_NOT_EQUAL(0, memcmp(a, d, sizeof(a)));
   /* Not a plain hash: unkeyed BLAKE2b of the input differs. */
   unsigned char plain[16];
   crypto_generichash(plain, sizeof(plain), (const unsigned char *)"k1", 2, NULL, 0);
   TEST_ASSERT_NOT_EQUAL(0, memcmp(a, plain, sizeof(a)));

   TEST_ASSERT_EQUAL_INT(1, crypto_store_keyed_digest("short", "k1", 2, a, sizeof(a)));
   TEST_ASSERT_EQUAL_INT(1, crypto_store_keyed_digest("test-ctx", "k1", 2, a, 8));
   TEST_ASSERT_EQUAL_INT(1, crypto_store_keyed_digest("test-ctx", NULL, 2, a, sizeof(a)));
}

static void test_key_tag(void) {
   char a[LLM_KEY_TAG_MAX], b[LLM_KEY_TAG_MAX], c[LLM_KEY_TAG_MAX];
   llm_key_tag("sk-one", a, sizeof(a));
   llm_key_tag("sk-one", b, sizeof(b));
   llm_key_tag("sk-two", c, sizeof(c));
   TEST_ASSERT_EQUAL_size_t(16, strlen(a));
   TEST_ASSERT_EQUAL_STRING(a, b);
   TEST_ASSERT_NOT_EQUAL(0, strcmp(a, c));
   TEST_ASSERT_EQUAL_size_t(strspn(a, "0123456789abcdef"), strlen(a));
   llm_key_tag(NULL, c, sizeof(c));
   TEST_ASSERT_EQUAL_size_t(16, strlen(c));
}

int main(void) {
   UNITY_BEGIN();

   RUN_TEST(test_init_succeeds);
   RUN_TEST(test_ready_after_init);
   RUN_TEST(test_roundtrip_string);
   RUN_TEST(test_roundtrip_binary);
   RUN_TEST(test_roundtrip_empty);
   RUN_TEST(test_encrypt_out_written);
   RUN_TEST(test_encrypt_out_written_null);
   RUN_TEST(test_encrypt_buffer_too_small);
   RUN_TEST(test_decrypt_buffer_too_small);
   RUN_TEST(test_decrypt_corrupt_data);
   RUN_TEST(test_decrypt_truncated);
   RUN_TEST(test_encrypt_null_input);
   RUN_TEST(test_encrypt_null_output);
   RUN_TEST(test_decrypt_null_input);
   RUN_TEST(test_decrypt_null_output);

   RUN_TEST(test_keyed_digest);
   RUN_TEST(test_key_tag);

   /* Shutdown must be last — pthread_once prevents re-init */
   RUN_TEST(test_shutdown_clears_ready);

   int result = UNITY_END();

   if (s_tmpdir_created) {
      char cmd[512];
      snprintf(cmd, sizeof(cmd), "rm -rf %s", s_tmpdir);
      system(cmd);
   }

   return result;
}
