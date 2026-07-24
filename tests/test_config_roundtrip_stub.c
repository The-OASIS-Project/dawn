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
 * Stubs for test_config_roundtrip: the config parse/write chain only reaches
 * out to the logger and the tool-registry config writer.  Logging is silenced
 * so the round-trip assertions are the only output; tool-owned sections (e.g.
 * [shutdown], [home_assistant]) are written through the registry and so are
 * deliberately out of scope for this test (see its section-coverage list).
 */

#include "test_config_roundtrip_stub.h"

#include <stdarg.h>

#include "logging.h"

/* Warnings are COUNTED, not just silenced.  config_parse_file() calls
 * warn_unknown_keys() for any key present in the file that the section's
 * known_keys[] doesn't list — which is precisely the FIELD-level half of the
 * bug class this test guards (writer emits a key the parser doesn't read).
 * The section-header check can't see that, and unlike the hand-maintained
 * required[] list this costs nothing to maintain: it works for every section,
 * present and future, automatically. */
static int s_warning_count;

void test_stub_reset_warnings(void) {
   s_warning_count = 0;
}

int test_stub_warning_count(void) {
   return s_warning_count;
}

void log_message(log_level_t level, const char *file, int line, const char *fmt, ...) {
   (void)file;
   (void)line;
   (void)fmt;
   if (level == LOGLEVEL_WARNING || level == LOGLEVEL_ERROR) {
      s_warning_count++;
   }
}

void tool_registry_write_configs(void *fp) {
   (void)fp;
}
