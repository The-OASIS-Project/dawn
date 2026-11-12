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
 * Stubs for test_memory_consolidate: an embedding provider in place of ONNX
 * (a bag of hashed words, so texts that share words are near, and unrelated
 * ones aren't), on only once a test calls embedding_engine_init, and the
 * history loader a queued extraction would use (the test never queues one).
 */

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "core/embedding_engine.h"
#include "memory/memory_history_loader.h"

#define WORDS_DIMS 256

static int words_init(const char *endpoint, const char *model, const char *api_key) {
   (void)endpoint;
   (void)model;
   (void)api_key;
   return 0;
}

static void words_cleanup(void) {
}

static int words_embed(const char *text, float *out, int max_dims, int *out_dims) {
   if (max_dims < WORDS_DIMS) {
      return 1;
   }
   memset(out, 0, WORDS_DIMS * sizeof(float));
   uint32_t h = 2166136261u;
   bool in_word = false;
   for (const char *c = text;; c++) {
      if (*c && isalnum((unsigned char)*c)) {
         h = (h ^ (uint32_t)tolower((unsigned char)*c)) * 16777619u;
         in_word = true;
      } else {
         if (in_word) {
            out[h % WORDS_DIMS] += 1.0f;
         }
         h = 2166136261u;
         in_word = false;
         if (!*c) {
            break;
         }
      }
   }
   *out_dims = WORDS_DIMS;
   return 0;
}

const embedding_provider_t embedding_provider_onnx = { .name = "onnx",
                                                       .init = words_init,
                                                       .cleanup = words_cleanup,
                                                       .embed = words_embed };

struct json_object *memory_history_load_from_db(int64_t conv_id, int user_id, size_t *len) {
   (void)conv_id;
   (void)user_id;
   if (len) {
      *len = 0;
   }
   return NULL;
}
