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
 * Stubs for tool_registry unit tests.
 *
 * Provides the minimal set of symbols that tool_registry.c references
 * at link time so the test binary can build without pulling in the full
 * daemon (LLM, MQTT, TOML parser, session manager, etc.).
 *
 * GUARD INVARIANT: every stub in this file MUST include the production header
 * that declares the stubbed symbol so that signature drift becomes a compile
 * error rather than a silent linker mismatch.
 *
 * MULTI-CLIENT GUARD: any session_manager_* stub MUST be wrapped in
 * `#ifdef ENABLE_MULTI_CLIENT`. When WEBUI is OFF (e.g., the `ci` preset),
 * core/session_manager.h provides static-inline fallbacks (see lines ~995–1114
 * of that header) and a non-inline definition here would collide. The same
 * pattern applies in test_plan_executor_stub.c.
 */

#include <stddef.h>
#include <stdio.h>

#include "config/dawn_config.h"
#include "core/device_types.h"
#include "core/session_manager.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_tools.h"
#include "tools/toml.h"

/* ============================================================================
 * Global config stubs (extern'd in dawn_config.h)
 * ============================================================================ */

dawn_config_t g_config = { 0 };
secrets_config_t g_secrets = { 0 };

/* ============================================================================
 * device_types.c — device_type_get_def() and pattern definitions
 *
 * tool_registry.c calls device_type_get_def() from count_device_type_patterns()
 * and uses action_count / pattern_count to compute variation totals.
 * Provide a minimal TRIGGER type so variation-count tests return non-zero.
 * ============================================================================ */

static const device_type_def_t STUB_TRIGGER = {
   .name = "trigger",
   .actions =
      {
         {
            .name = "trigger",
            .patterns = {"trigger %device_name%", "%device_name%"},
            .pattern_count = 2,
         },
      },
   .action_count = 1,
};

static const device_type_def_t STUB_BOOLEAN = {
   .name = "boolean",
   .actions =
      {
         {
            .name = "enable",
            .patterns = {"enable %device_name%", "turn on %device_name%"},
            .pattern_count = 2,
         },
         {
            .name = "disable",
            .patterns = {"disable %device_name%", "turn off %device_name%"},
            .pattern_count = 2,
         },
      },
   .action_count = 2,
};

const device_type_def_t *device_type_get_def(tool_device_type_t type) {
   switch (type) {
      case TOOL_DEVICE_TYPE_TRIGGER:
         return &STUB_TRIGGER;
      case TOOL_DEVICE_TYPE_BOOLEAN:
         return &STUB_BOOLEAN;
      default:
         return NULL;
   }
}

/* ============================================================================
 * llm_tools.c — llm_tools_invalidate_cache()
 *
 * Called from tool_registry_invalidate_cache(). No-op for tests.
 * ============================================================================ */

void llm_tools_invalidate_cache(void) {
}

/* ============================================================================
 * llm_command_parser.c — invalidate_system_instructions()
 *
 * Called from tool_registry_invalidate_cache(). No-op for tests.
 * ============================================================================ */

void invalidate_system_instructions(void) {
}

/* ============================================================================
 * TOML stubs — toml_parse_file / toml_table_in / toml_free
 *
 * tool_registry_parse_configs() calls these. The test does not exercise
 * config parsing, so returning NULL / no-op is safe. Signatures match
 * tools/toml.h (included at the top of this file).
 * ============================================================================ */

toml_table_t *toml_parse_file(FILE *fp, char *errbuf, int errbufsz) {
   (void)fp;
   if (errbuf && errbufsz > 0)
      errbuf[0] = '\0';
   return NULL;
}

toml_table_t *toml_table_in(const toml_table_t *tab, const char *key) {
   (void)tab;
   (void)key;
   return NULL;
}

void toml_free(toml_table_t *tab) {
   (void)tab;
}

/* ============================================================================
 * llm_interface.c — llm_get_default_config()
 *
 * Referenced by the session_get_local() inline stub in session_manager.h
 * (local-only mode). Provide a minimal implementation so the linker is
 * satisfied if the compiler instantiates that inline function.
 * ============================================================================ */

#include "llm/llm_interface.h"

void llm_get_default_config(session_llm_config_t *config) {
   if (config) {
      config->type = LLM_LOCAL;
      config->cloud_provider = CLOUD_PROVIDER_NONE;
      config->endpoint[0] = '\0';
      config->model[0] = '\0';
      config->suppress_tools = false;
      config->thinking_mode[0] = '\0';
      config->reasoning_effort[0] = '\0';
   }
}

/* ============================================================================
 * config_parser.c — config_get / config_get_secrets
 * ============================================================================ */

const dawn_config_t *config_get(void) {
   return &g_config;
}

const secrets_config_t *config_get_secrets(void) {
   return &g_secrets;
}

/* ============================================================================
 * session_manager.c — session_manager_refresh_all_prompts
 *
 * Only defined when ENABLE_MULTI_CLIENT is on. In local-only builds (e.g., the
 * `ci` preset with WEBUI=OFF), session_manager.h provides an inline stub, and
 * defining a non-inline version here would collide.
 * ============================================================================ */

#ifdef ENABLE_MULTI_CLIENT
void session_manager_refresh_all_prompts(void) {
}
#endif
