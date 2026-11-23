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
 * Stub providing auth_db_state_t s_db for document_db unit tests.
 * Allows document_db.c to link without pulling in auth_db_core.c.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"

auth_db_state_t s_db = {
   .db = NULL,
   .mutex = PTHREAD_MUTEX_INITIALIZER,
   .initialized = false,
};

/* document_db.c reads g_config.documents.version_retention_days (v62 versioning).
 * Zero-init here = versioning disabled, so non-versioning tests are unaffected;
 * versioning tests set the field explicitly in setUp(). */
dawn_config_t g_config;

/* document_read.c (its gap-safe pagination is tested here, over the same
 * in-memory documents): the registry, the caller's user and the framing call. */
#include <stdbool.h>
#include <stddef.h>

#include "core/session_manager.h"
#include "dawn_error.h"
#include "tools/tool_registry.h"

const tool_metadata_t *g_test_registered_tool = NULL;

int tool_registry_register(const tool_metadata_t *metadata) {
   g_test_registered_tool = metadata;
   return SUCCESS;
}

session_t *session_get_command_context(void) {
   return NULL;
}

int session_effective_user_id(session_t *session) {
   (void)session;
   return 0;
}

int session_default_voice_user_id(void) {
   return 1; /* alice */
}

bool scheduled_context_get(int *user_id_out) {
   (void)user_id_out;
   return false;
}

bool embedding_engine_available(void) {
   return true;
}

int g_test_frame_calls = 0;

void llm_tools_result_third_party(const char *frame) {
   (void)frame;
   g_test_frame_calls++;
}
