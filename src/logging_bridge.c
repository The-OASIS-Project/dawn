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
 * Logging Bridge - Implementation
 */

#include "logging_bridge.h"

#include <stdarg.h>
#include <stdio.h>

#include "logging.h"
#include "logging_common.h"

/**
 * @brief Bridge callback that forwards common library logs to daemon logging
 *
 * Translates from dawn_log_level_t to log_level_t and calls log_message().
 */
static void logging_bridge_callback(dawn_log_level_t level,
                                    const char *file,
                                    int line,
                                    const char *func,
                                    const char *fmt,
                                    va_list args) {
   /* Map dawn_log_level_t to log_level_t */
   log_level_t daemon_level;
   switch (level) {
      case DAWN_LOG_WARNING:
         daemon_level = LOG_WARNING;
         break;
      case DAWN_LOG_ERROR:
         daemon_level = LOG_ERROR;
         break;
      case DAWN_LOG_INFO:
      default:
         daemon_level = LOG_INFO;
         break;
   }

   /* Format the message using the variadic arguments */
   char message[1024];
   vsnprintf(message, sizeof(message), fmt, args);

   /* Call daemon's log_message with the pre-formatted message */
   log_message(daemon_level, file, line, func, "%s", message);
}

void logging_bridge_init(void) {
   dawn_common_set_logger(logging_bridge_callback);
}
