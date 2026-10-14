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
 * Stubs for test_memory_remember and test_memory_supersede_kinds: the ONNX
 * embedding provider (the tests never embed, and linking the real provider
 * would need onnxruntime, which CI lacks), and the history loader a queued
 * extraction re-reads its conversation with (the tests never queue one).
 */

#include <stddef.h>
#include <stdint.h>

#include "core/embedding_engine.h"
#include "memory/memory_history_loader.h"

const embedding_provider_t embedding_provider_onnx = { .name = "onnx" };

struct json_object *memory_history_load_from_db(int64_t conv_id, int user_id, size_t *len) {
   (void)conv_id;
   (void)user_id;
   if (len) {
      *len = 0;
   }
   return NULL;
}
