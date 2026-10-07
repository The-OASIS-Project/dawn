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
 * OTA signed-manifest core — pure, libsodium-only.  See ota_manifest.h.
 *
 * SATELLITE-SAFE CONTRACT: this file is compiled into BOTH the daemon and the
 * dawn_satellite Tier-1 binary (via dawn_satellite/CMakeLists.txt).  It MUST
 * stay free of daemon globals (g_config/g_secrets), auth_db, file I/O, and any
 * non-libsodium/libc dependency, or the satellite build breaks.  Keep it pure.
 */

#include "core/ota_manifest.h"

#include <sodium.h>
#include <stdlib.h>
#include <string.h>

/* Keep the header's mirrored sizes honest against libsodium. */
_Static_assert(OTA_SIG_BYTES == crypto_sign_BYTES, "sig size drift");
_Static_assert(OTA_PK_BYTES == crypto_sign_PUBLICKEYBYTES, "pk size drift");
_Static_assert(OTA_SK_BYTES == crypto_sign_SECRETKEYBYTES, "sk size drift");
_Static_assert(OTA_SHA256_BYTES == crypto_hash_sha256_BYTES, "sha256 size drift");

#define MAGIC_LEN 8

int ota_crypto_init(void) {
   /* sodium_init: 0 = success, 1 = already initialized, -1 = failure. */
   return (sodium_init() < 0) ? FAILURE : SUCCESS;
}

void ota_keygen(uint8_t pk[OTA_PK_BYTES], uint8_t sk[OTA_SK_BYTES]) {
   crypto_sign_keypair(pk, sk);
}

int ota_sha256(const uint8_t *data, size_t len, uint8_t out[OTA_SHA256_BYTES]) {
   if ((data == NULL && len != 0) || out == NULL) {
      return FAILURE;
   }
   crypto_hash_sha256(out, data, len);
   return SUCCESS;
}

/* --- little-endian field writers/readers --- */

static void put_u16le(uint8_t *p, uint16_t v) {
   p[0] = (uint8_t)(v & 0xff);
   p[1] = (uint8_t)((v >> 8) & 0xff);
}

static uint16_t get_u16le(const uint8_t *p) {
   return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void put_u64le(uint8_t *p, uint64_t v) {
   for (int i = 0; i < 8; i++) {
      p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
   }
}

static uint64_t get_u64le(const uint8_t *p) {
   uint64_t v = 0;
   for (int i = 0; i < 8; i++) {
      v |= ((uint64_t)p[i]) << (8 * i);
   }
   return v;
}

/* Zero-fill a fixed string field then copy at most cap-bounded bytes, so the
 * serialized blob is canonical (no uninitialized bytes after the NUL feed into
 * the signature). */
static void put_str(uint8_t *p, size_t field_size, const char *s) {
   memset(p, 0, field_size);
   if (s != NULL) {
      size_t n = strnlen(s, field_size); /* drop anything that wouldn't round-trip */
      memcpy(p, s, n);
   }
}

int ota_manifest_serialize(const ota_manifest_t *m,
                           uint8_t *out,
                           size_t out_size,
                           size_t *out_len) {
   if (m == NULL || out == NULL || out_size < OTA_MANIFEST_WIRE_SIZE) {
      return FAILURE;
   }

   uint8_t *p = out;
   /* fixed-width field of the binary manifest, never a C string */
   // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
   memcpy(p, OTA_MANIFEST_MAGIC, MAGIC_LEN);
   p += MAGIC_LEN;
   put_u16le(p, m->fmt_version);
   p += 2;
   *p++ = m->platform;
   *p++ = m->tier;
   put_str(p, OTA_VERSION_MAX, m->version);
   p += OTA_VERSION_MAX;
   put_str(p, OTA_ABI_TAG_MAX, m->abi_tag);
   p += OTA_ABI_TAG_MAX;
   put_u64le(p, m->image_size);
   p += 8;
   memcpy(p, m->sha256, OTA_SHA256_BYTES);
   p += OTA_SHA256_BYTES;
   put_str(p, OTA_VERSION_MAX, m->min_version);
   p += OTA_VERSION_MAX;

   if (out_len != NULL) {
      *out_len = (size_t)(p - out);
   }
   return SUCCESS;
}

int ota_manifest_deserialize(const uint8_t *buf, size_t len, ota_manifest_t *out) {
   if (buf == NULL || out == NULL || len < OTA_MANIFEST_WIRE_SIZE) {
      return FAILURE;
   }
   if (memcmp(buf, OTA_MANIFEST_MAGIC, MAGIC_LEN) != 0) {
      return FAILURE;
   }

   memset(out, 0, sizeof(*out));
   const uint8_t *p = buf + MAGIC_LEN;
   out->fmt_version = get_u16le(p);
   p += 2;
   if (out->fmt_version != OTA_MANIFEST_FMT_VERSION) {
      return FAILURE;
   }
   out->platform = *p++;
   out->tier = *p++;
   /* String fields are NUL-padded on the wire; copy field_size-1 max and force
    * termination so a non-terminated field can never overrun a reader. */
   memcpy(out->version, p, OTA_VERSION_MAX);
   out->version[OTA_VERSION_MAX - 1] = '\0';
   p += OTA_VERSION_MAX;
   memcpy(out->abi_tag, p, OTA_ABI_TAG_MAX);
   out->abi_tag[OTA_ABI_TAG_MAX - 1] = '\0';
   p += OTA_ABI_TAG_MAX;
   out->image_size = get_u64le(p);
   p += 8;
   memcpy(out->sha256, p, OTA_SHA256_BYTES);
   p += OTA_SHA256_BYTES;
   memcpy(out->min_version, p, OTA_VERSION_MAX);
   out->min_version[OTA_VERSION_MAX - 1] = '\0';

   return SUCCESS;
}

int ota_manifest_sign(const ota_manifest_t *m,
                      const uint8_t sk[OTA_SK_BYTES],
                      uint8_t *out,
                      size_t out_size,
                      size_t *out_len,
                      uint8_t sig[OTA_SIG_BYTES]) {
   if (sk == NULL || sig == NULL) {
      return FAILURE;
   }
   size_t len = 0;
   if (ota_manifest_serialize(m, out, out_size, &len) != SUCCESS) {
      return FAILURE;
   }
   if (crypto_sign_detached(sig, NULL, out, len, sk) != 0) {
      return FAILURE;
   }
   if (out_len != NULL) {
      *out_len = len;
   }
   return SUCCESS;
}

int ota_manifest_verify(const uint8_t *raw,
                        size_t raw_len,
                        const uint8_t sig[OTA_SIG_BYTES],
                        const uint8_t (*pubkeys)[OTA_PK_BYTES],
                        size_t n_keys,
                        ota_manifest_t *out) {
   if (raw == NULL || sig == NULL || pubkeys == NULL || n_keys == 0) {
      return FAILURE;
   }

   /* Verify the EXACT received bytes against the keyring BEFORE any parsing. */
   bool ok = false;
   for (size_t i = 0; i < n_keys; i++) {
      if (crypto_sign_verify_detached(sig, raw, raw_len, pubkeys[i]) == 0) {
         ok = true;
         break;
      }
   }
   if (!ok) {
      return FAILURE;
   }

   /* Signature is good — now it's safe to parse.  out is optional. */
   if (out != NULL) {
      return ota_manifest_deserialize(raw, raw_len, out);
   }
   /* Even when the caller doesn't want the parsed struct, confirm the bytes are
    * a well-formed manifest so a valid signature over garbage is rejected. */
   ota_manifest_t scratch;
   return ota_manifest_deserialize(raw, raw_len, &scratch);
}

/* Parse "MAJOR.MINOR.PATCH" (extra dot-components and any pre-release/build
 * suffix are ignored).  Missing trailing components default to 0. */
static int parse_semver(const char *s, long out[3]) {
   if (s == NULL || s[0] == '\0') {
      return FAILURE;
   }
   out[0] = out[1] = out[2] = 0;
   const char *p = s;
   for (int i = 0; i < 3; i++) {
      char *end = NULL;
      long v = strtol(p, &end, 10);
      if (end == p || v < 0) {
         return FAILURE; /* non-numeric component */
      }
      out[i] = v;
      if (*end == '.') {
         p = end + 1;
      } else {
         break; /* end of numeric version (NUL or suffix like "-rc1") */
      }
   }
   return SUCCESS;
}

int ota_semver_compare(const char *a, const char *b, int *result) {
   long va[3], vb[3];
   if (parse_semver(a, va) != SUCCESS || parse_semver(b, vb) != SUCCESS) {
      return FAILURE;
   }
   int cmp = 0;
   for (int i = 0; i < 3 && cmp == 0; i++) {
      if (va[i] < vb[i]) {
         cmp = -1;
      } else if (va[i] > vb[i]) {
         cmp = 1;
      }
   }
   if (result != NULL) {
      *result = cmp;
   }
   return SUCCESS;
}

bool ota_manifest_rollback_ok(const char *installed,
                              const ota_manifest_t *m,
                              bool allow_downgrade) {
   if (m == NULL) {
      return false;
   }
   /* No known installed version (fresh device / unparseable) — allow; the
    * downgrade/floor checks below need a baseline to compare against. */
   int cmp = 0;
   if (installed != NULL && installed[0] != '\0' &&
       ota_semver_compare(m->version, installed, &cmp) == SUCCESS) {
      if (cmp < 0 && !allow_downgrade) {
         return false; /* candidate older than installed, downgrade not allowed */
      }
   }
   /* Anti-skip floor: the device must already be at >= min_version. */
   if (m->min_version[0] != '\0' && installed != NULL && installed[0] != '\0') {
      int fcmp = 0;
      if (ota_semver_compare(installed, m->min_version, &fcmp) == SUCCESS && fcmp < 0) {
         return false;
      }
   }
   return true;
}
