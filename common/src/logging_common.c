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
 * Common Logging Interface - Implementation
 */

#include "logging_common.h"

#include <stdarg.h>
#include <stdio.h>

/* Global logging callback - set by application */
static dawn_log_callback_t g_log_callback = NULL;

void dawn_common_set_logger(dawn_log_callback_t callback) {
   g_log_callback = callback;
}

void dawn_common_log(dawn_log_level_t level,
                     const char *file,
                     int line,
                     const char *func,
                     const char *fmt,
                     ...) {
   if (!g_log_callback) {
      /* No callback registered - discard log message */
      return;
   }

   va_list args;
   va_start(args, fmt);
   g_log_callback(level, file, line, func, fmt, args);
   va_end(args);
}
