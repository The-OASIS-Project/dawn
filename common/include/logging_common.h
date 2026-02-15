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
 * Common Logging Interface - Callback-based logging for shared library code
 *
 * This header provides a callback-based logging abstraction that allows
 * common library code to log messages without depending on daemon-specific
 * logging infrastructure. The daemon (or satellite) registers its logging
 * callback at initialization.
 */

#ifndef DAWN_COMMON_LOGGING_COMMON_H
#define DAWN_COMMON_LOGGING_COMMON_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Log level enumeration (matches daemon's log_level_t)
 */
typedef enum {
   DAWN_LOG_INFO = 0,
   DAWN_LOG_WARNING = 1,
   DAWN_LOG_ERROR = 2,
} dawn_log_level_t;

/**
 * @brief Callback function type for logging
 *
 * The application (daemon or satellite) provides this callback to handle
 * log messages from the common library.
 *
 * @param level Log level (DAWN_LOG_INFO, DAWN_LOG_WARNING, DAWN_LOG_ERROR)
 * @param file Source file name (from __FILE__)
 * @param line Line number (from __LINE__)
 * @param func Function name (from __func__)
 * @param fmt Printf-style format string
 * @param args Variable arguments list
 */
typedef void (*dawn_log_callback_t)(dawn_log_level_t level,
                                    const char *file,
                                    int line,
                                    const char *func,
                                    const char *fmt,
                                    va_list args);

/**
 * @brief Set the logging callback for common library code
 *
 * Must be called by the application before using any common library
 * functions that log. If not set, log messages are discarded.
 *
 * Thread Safety: This function is NOT thread-safe. Call it once at
 * initialization before any other threads are started.
 *
 * @param callback The logging callback function, or NULL to disable logging
 */
void dawn_common_set_logger(dawn_log_callback_t callback);

/**
 * @brief Internal logging function - do not call directly
 *
 * Use the DAWN_LOG_* macros instead.
 */
void dawn_common_log(dawn_log_level_t level,
                     const char *file,
                     int line,
                     const char *func,
                     const char *fmt,
                     ...);

/**
 * @brief Macro for logging informational messages
 *
 * @param fmt Printf-style format string
 * @param ... Additional arguments for the format string
 */
#define DAWN_LOG_INFO(fmt, ...) \
   dawn_common_log(DAWN_LOG_INFO, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

/**
 * @brief Macro for logging warning messages
 *
 * @param fmt Printf-style format string
 * @param ... Additional arguments for the format string
 */
#define DAWN_LOG_WARNING(fmt, ...) \
   dawn_common_log(DAWN_LOG_WARNING, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

/**
 * @brief Macro for logging error messages
 *
 * @param fmt Printf-style format string
 * @param ... Additional arguments for the format string
 */
#define DAWN_LOG_ERROR(fmt, ...) \
   dawn_common_log(DAWN_LOG_ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* DAWN_COMMON_LOGGING_COMMON_H */
