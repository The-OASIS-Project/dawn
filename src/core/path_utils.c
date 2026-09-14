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

#include "core/path_utils.h"

#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "logging.h"

/* safe_strncpy() moved to the shared string utilities (utils/string_utils.h),
 * reachable here via core/path_utils.h. */

/* =============================================================================
 * Path Expansion
 * ============================================================================= */

bool path_expand_tilde(const char *path, char *expanded, size_t expanded_size) {
   if (!path || !expanded || expanded_size == 0) {
      return false;
   }

   /* Check for tilde prefix: must be ~ followed by / or end of string */
   if (path[0] != '~' || (path[1] != '/' && path[1] != '\0')) {
      /* No tilde expansion needed - copy as-is */
      safe_strncpy(expanded, path, expanded_size);
      return true;
   }

   /* Get home directory from environment first (faster) */
   const char *home = getenv("HOME");
   if (!home) {
      /* Fall back to passwd database */
      struct passwd *pw = getpwuid(getuid());
      if (pw) {
         home = pw->pw_dir;
      }
   }

   if (!home) {
      OLOG_ERROR("path_expand_tilde: HOME not set and getpwuid failed");
      return false;
   }

   /* Build expanded path: home + suffix after ~ */
   const char *suffix = (path[1] == '/') ? path + 2 : "";
   int written = snprintf(expanded, expanded_size, "%s/%s", home, suffix);
   if (written < 0 || (size_t)written >= expanded_size) {
      OLOG_ERROR("path_expand_tilde: Expanded path too long");
      return false;
   }

   return true;
}

/* =============================================================================
 * Path Canonicalization
 * ============================================================================= */

bool path_canonicalize(const char *path, char *canonical, size_t canonical_size) {
   if (!path || !canonical || canonical_size == 0) {
      return false;
   }

   /* First expand tilde if present */
   char expanded[PATH_MAX];
   if (!path_expand_tilde(path, expanded, sizeof(expanded))) {
      return false;
   }

   /* Use realpath to resolve symlinks and relative components */
   char resolved[PATH_MAX];
   if (!realpath(expanded, resolved)) {
      /* Path doesn't exist or can't be resolved */
      return false;
   }

   /* Check if result fits in output buffer */
   size_t len = strlen(resolved);
   if (len >= canonical_size) {
      OLOG_ERROR("path_canonicalize: Canonical path too long for buffer");
      return false;
   }

   memcpy(canonical, resolved, len + 1);
   return true;
}

/* =============================================================================
 * Path Validation
 * ============================================================================= */

bool path_is_within_root(const char *path, const char *root_dir) {
   if (!path || !root_dir) {
      return false;
   }

   /* Canonicalize both paths to resolve symlinks and .. */
   char canonical_path[PATH_MAX];
   char canonical_root[PATH_MAX];

   if (!path_canonicalize(path, canonical_path, sizeof(canonical_path))) {
      return false;
   }

   if (!path_canonicalize(root_dir, canonical_root, sizeof(canonical_root))) {
      return false;
   }

   /* Check that path starts with root */
   size_t root_len = strlen(canonical_root);
   if (strncmp(canonical_path, canonical_root, root_len) != 0) {
      return false;
   }

   /* Ensure it's not just a prefix match (e.g., /music vs /music2) */
   /* Path must either equal root exactly, or have / after root prefix */
   if (canonical_path[root_len] != '\0' && canonical_path[root_len] != '/') {
      return false;
   }

   return true;
}

/* =============================================================================
 * Directory Creation
 * ============================================================================= */

bool path_ensure_parent_dir(const char *file_path) {
   return path_ensure_parent_dir_mode(file_path, 0755);
}

bool path_ensure_parent_dir_mode(const char *file_path, mode_t mode) {
   if (!file_path || file_path[0] == '\0') {
      return false;
   }

   /* dirname() may modify its argument, so make a copy */
   char *path_copy = strdup(file_path);
   if (!path_copy) {
      OLOG_ERROR("path_ensure_parent_dir: strdup failed");
      return false;
   }

   char *dir = dirname(path_copy);

   /* Fast path: try creating the leaf directory first (common case where only
    * the final component is missing, avoids walking the entire path) */
   if (mkdir(dir, mode) == 0) {
      OLOG_INFO("Created directory: %s", dir);
      free(path_copy);
      return true;
   }

   if (errno == EEXIST) {
      /* Verify it's actually a directory (use lstat to not follow symlinks) */
      struct stat st;
      if (lstat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
         free(path_copy);
         return true;
      }
      OLOG_ERROR("path_ensure_parent_dir: '%s' exists but is not a directory", dir);
      free(path_copy);
      return false;
   }

   /* Fast path failed (missing intermediate dirs) — walk the path and create
    * each component.  This handles fresh installs where e.g. ~/.local/share/
    * doesn't exist yet. */
   char *p = dir;
   if (*p == '/') {
      p++; /* skip leading slash */
   }

   for (; *p; p++) {
      if (*p == '/') {
         *p = '\0';
         if (mkdir(dir, mode) != 0 && errno != EEXIST) {
            OLOG_ERROR("path_ensure_parent_dir: failed to create '%s': %s", dir, strerror(errno));
            free(path_copy);
            return false;
         }
         *p = '/';
      }
   }

   /* Create the final component */
   if (mkdir(dir, mode) != 0 && errno != EEXIST) {
      OLOG_ERROR("path_ensure_parent_dir: failed to create '%s': %s", dir, strerror(errno));
      free(path_copy);
      return false;
   }

   OLOG_INFO("Created directory: %s", dir);
   free(path_copy);
   return true;
}
