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
 * Path Utilities - Common path manipulation functions
 */

#ifndef DAWN_PATH_UTILS_H
#define DAWN_PATH_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* safe_strncpy() is the project's canonical bounded string copy; it lives in the
 * shared string utilities.  Included here so existing path_utils.h consumers that
 * call safe_strncpy() keep compiling after the definition was de-duplicated. */
#include "utils/string_utils.h"

/**
 * @brief Expand tilde in path to home directory
 *
 * Handles paths like "~/Music" -> "/home/user/Music"
 * Falls back to getpwuid() if HOME environment variable is not set.
 *
 * @param path Original path (may contain leading ~/)
 * @param expanded Output buffer for expanded path
 * @param expanded_size Size of output buffer
 * @return true if expansion occurred or path copied unchanged, false on error
 *
 * @note Thread-safe. Uses getenv() and getpwuid() which are MT-safe on Linux.
 */
bool path_expand_tilde(const char *path, char *expanded, size_t expanded_size);

/**
 * @brief Canonicalize path by resolving symlinks and relative components
 *
 * Wrapper around realpath() with additional validation.
 * Resolves "..", ".", and symbolic links to produce an absolute path.
 *
 * @param path Path to canonicalize (may contain ~/, .., symlinks)
 * @param canonical Output buffer for canonical path
 * @param canonical_size Size of output buffer
 * @return true on success, false if path doesn't exist or buffer too small
 *
 * @note The path must exist for canonicalization to succeed.
 */
bool path_canonicalize(const char *path, char *canonical, size_t canonical_size);

/**
 * @brief Check if a path is within a specified root directory
 *
 * Uses canonicalization to prevent symlink and ".." escape attacks.
 * Both paths are canonicalized before comparison.
 *
 * @param path Path to check
 * @param root_dir Root directory that path must be within
 * @return true if path is within root_dir, false otherwise
 *
 * @note Both paths must exist for this check to succeed.
 */
bool path_is_within_root(const char *path, const char *root_dir);

/**
 * @brief Ensure parent directory exists for a file path
 *
 * Recursively creates all missing parent directories for the given file path.
 * Uses mode 0755 for directories.
 *
 * @param file_path Path to a file (parent directories will be created)
 * @return true on success or if directory already exists, false on error
 *
 * @note The file_path should be an expanded absolute path.
 */
bool path_ensure_parent_dir(const char *file_path);

/**
 * @brief Ensure parent directory exists with specific permissions
 *
 * Like path_ensure_parent_dir() but allows specifying the directory mode.
 * Useful for creating directories with restrictive permissions (e.g. 0700
 * for credential storage).
 *
 * @param file_path Path to a file (parent directories will be created)
 * @param mode      Permission mode for created directories (e.g. 0700)
 * @return true on success or if directory already exists, false on error
 */
bool path_ensure_parent_dir_mode(const char *file_path, mode_t mode);

#endif /* DAWN_PATH_UTILS_H */
