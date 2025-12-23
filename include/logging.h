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
 */

/**
 * @file logging.h
 * @brief Logging system for recording messages with varying severity levels.
 *
 * This header defines the logging system, including log levels, initialization,
 * and functions for logging messages with contextual information such as file name,
 * line number, and function name.
 */

#ifndef LOGGING_H
#define LOGGING_H

#include <stdarg.h> /* For va_list */
#include <stdio.h>  /* For FILE */

/**
 * @brief Log level enumeration for specifying the severity of log messages.
 */
typedef enum {
   LOG_INFO,    /**< Informational messages that represent normal operation. */
   LOG_WARNING, /**< Warning messages indicating potential issues. */
   LOG_ERROR,   /**< Error messages indicating failures or critical issues. */
} log_level_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Logs a formatted message with a specified log level and context information.
 *
 * This function logs a message with the given format and arguments, including the file name,
 * line number, and function name for context. It supports variable arguments similar to `printf`.
 *
 * @param level The log level indicating the severity of the message (e.g., `LOG_INFO`,
 * `LOG_WARNING`, `LOG_ERROR`).
 * @param file  The name of the source file where the log function was called (usually `__FILE__`).
 * @param line  The line number in the source file where the log function was called (usually
 * `__LINE__`).
 * @param func  The name of the function where the log function was called (usually `__func__`).
 * @param fmt   The format string for the log message, similar to `printf`.
 * @param ...   Additional arguments for the format string.
 */
void log_message(log_level_t level,
                 const char *file,
                 int line,
                 const char *func,
                 const char *fmt,
                 ...);

#ifdef __cplusplus
}
#endif

/**
 * @brief Initializes the logging system.
 *
 * This function initializes the logging system and sets up logging to either a file or the console.
 * It should be called before any logging functions are used.
 *
 * @param filename The name of the log file to write to. If `NULL` or empty, and LOG_TO_FILE, error.
 * @param to_file  An integer flag indicating where to log messages:
 *                 - `LOG_TO_CONSOLE` (0): Log messages to the console (stdout/stderr).
 *                 - `LOG_TO_FILE` (1): Log messages to the specified file.
 *
 * @return Returns `0` on success, or a non-zero error code on failure.
 */
int init_logging(const char *filename, int to_file);

/**
 * @brief Closes the logging system.
 *
 * This function closes the logging system and releases any resources allocated during
 * initialization. It should be called when logging is no longer needed, typically at the end of the
 * application.
 */
void close_logging(void);

/**
 * @brief Suppress console logging (for TUI mode).
 *
 * When enabled, log messages will only go to file (if configured).
 * Console output is suppressed to avoid interfering with TUI display.
 *
 * @param suppress 1 to suppress console logging, 0 to enable
 */
void logging_suppress_console(int suppress);

/**
 * @def LOG_TO_CONSOLE
 * @brief Macro indicating that logs should be output to the console.
 */
#define LOG_TO_CONSOLE 0

/**
 * @def LOG_TO_FILE
 * @brief Macro indicating that logs should be output to a file.
 */
#define LOG_TO_FILE 1

/**
 * @def MAX_LOG_LENGTH
 * @brief Maximum length of each line of the log messages
 */
#define MAX_LOG_LENGTH 1024

/**
 * @brief Macro for logging informational messages.
 *
 * This macro simplifies logging of informational messages by automatically including the file name,
 * line number, and function name where the macro is called.
 *
 * @param fmt  The format string for the log message.
 * @param ...  Additional arguments for the format string.
 */
#define LOG_INFO(fmt, ...) log_message(LOG_INFO, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

/**
 * @brief Macro for logging warning messages.
 *
 * This macro simplifies logging of warning messages by automatically including the file name,
 * line number, and function name where the macro is called.
 *
 * @param fmt  The format string for the log message.
 * @param ...  Additional arguments for the format string.
 */
#define LOG_WARNING(fmt, ...) \
   log_message(LOG_WARNING, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

/**
 * @brief Macro for logging error messages.
 *
 * This macro simplifies logging of error messages by automatically including the file name,
 * line number, and function name where the macro is called.
 *
 * @param fmt  The format string for the log message.
 * @param ...  Additional arguments for the format string.
 */
#define LOG_ERROR(fmt, ...) log_message(LOG_ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

/**
 * @brief Macro for safely logging credential/API key status
 *
 * This macro prevents accidental logging of actual credentials by converting
 * any non-NULL pointer to a safe status string. Use this instead of directly
 * logging API keys or tokens.
 *
 * @param key  Pointer to credential (API key, token, etc.)
 * @return     "(configured)" if key is non-NULL, "(not configured)" otherwise
 */
#define LOG_CREDENTIAL_STATUS(key) ((key) ? "(configured)" : "(not configured)")

#endif  // LOGGING_H
