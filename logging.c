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

#include "logging.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

// Global variable for the log file
static FILE *log_file = NULL;

// ANSI color codes
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_RESET   "\x1b[0m"

// Fixed width for the preamble
#define PREAMBLE_WIDTH 45

// Get current timestamp with milliseconds
static void get_timestamp_ms(char *buffer, size_t buffer_size) {
    struct timeval tv;
    struct tm *tm_info;

    gettimeofday(&tv, NULL);
    tm_info = localtime(&tv.tv_sec);

    // Format: HH:MM:SS.mmm
    snprintf(buffer, buffer_size, "%02d:%02d:%02d.%03d",
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec,
             (int)(tv.tv_usec / 1000));
}

// Utility function to get the filename from the path
static const char *get_filename(const char *path) {
    const char *filename = strrchr(path, '/');
    if (!filename) {
        filename = strrchr(path, '\\');
    }
    return filename ? filename + 1 : path;
}

// Utility function to remove newlines from a string
static void remove_newlines(char *str) {
    char *src = str, *dst = str;
    while (*src) {
        if (*src != '\n' && *src != '\r') {
            *dst++ = *src;
        }
        src++;
    }
    *dst = '\0';
}

// Logging function implementation
void log_message(log_level_t level, const char *file, int line, const char *func, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    const char *level_str = NULL;
    const char *color_code = NULL;
    FILE *output_stream = log_file ? log_file : stdout;

    switch (level) {
        case LOG_INFO:
            level_str = "INFO";
            color_code = ANSI_COLOR_GREEN;
            break;
        case LOG_WARNING:
            level_str = "WARN";
            color_code = ANSI_COLOR_YELLOW;
            break;
        case LOG_ERROR:
            level_str = "ERR ";
            color_code = ANSI_COLOR_RED;
            output_stream = log_file ? log_file : stderr;
            break;
        default:
            va_end(args);
            return;
    }

    const char *filename = get_filename(file);

    // Get timestamp
    char timestamp[13]; // "HH:MM:SS.mmm"
    get_timestamp_ms(timestamp, sizeof(timestamp));

    // Create the preamble
    char preamble[PREAMBLE_WIDTH + 5];
    int preamble_length = snprintf(preamble, sizeof(preamble), "[%s] %s %s:%d: ",
                              level_str, timestamp, filename, line);

    // Pad to fixed width for alignment
    if (preamble_length < PREAMBLE_WIDTH) {
        int padding_length = PREAMBLE_WIDTH - preamble_length;
        memset(preamble + preamble_length, ' ', padding_length);
        preamble[PREAMBLE_WIDTH] = '\0';
    }

    if (log_file) {
        // Log to file without colors
        fprintf(output_stream, "%s", preamble);
    } else {
        // Log to console with colors
        fprintf(output_stream, "%s%s", color_code, preamble);
    }

    // Prepare the log message
    char log_message[MAX_LOG_LENGTH];
    vsnprintf(log_message, sizeof(log_message), fmt, args);
    remove_newlines(log_message);

    // Print the log message
    fprintf(output_stream, "%s", log_message);
    if (!log_file) {
        fprintf(output_stream, "%s", ANSI_COLOR_RESET);
    }
    fprintf(output_stream, "\n");

    va_end(args);
}

// Initialization function implementation
int init_logging(const char *filename, int to_file) {
    // Close the previous log file if open
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }

    if (to_file) {
        if (filename) {
            log_file = fopen(filename, "w");
            if (!log_file) {
                fprintf(stderr, "Failed to open log file: %s\n", filename);
                return -1;
            }
        } else {
            fprintf(stderr, "Filename cannot be NULL when mode is LOG_TO_FILE\n");
            return -1;
        }
    }

    return 0;
}

// Close logging function implementation
void close_logging(void) {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}

