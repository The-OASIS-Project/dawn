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
 * Generic instruction loader for the two-step tool pattern.
 * Reads markdown files from disk, concatenates _core.md + requested modules,
 * and returns the content for LLM context injection.
 */

#include "tools/instruction_loader.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "dawn_error.h"
#include "logging.h"

#define SEPARATOR "\n\n---\n\n"
#define SEPARATOR_LEN 7

/**
 * @brief Validate a module name for path traversal safety
 *
 * Rejects names containing '/', '\', '..', or starting with '.'.
 * Module names should be simple identifiers like "diagram" or "chart".
 */
static bool is_safe_module_name(const char *name) {
   if (!name || name[0] == '\0' || name[0] == '.') {
      return false;
   }

   for (const char *p = name; *p != '\0'; p++) {
      if (*p == '/' || *p == '\\') {
         return false;
      }
      /* Check for ".." anywhere in the name */
      if (*p == '.' && *(p + 1) == '.') {
         return false;
      }
   }

   return true;
}

/**
 * @brief Read a file into a buffer, appending at offset
 *
 * Uses stat() to determine file size, checks against remaining capacity,
 * reads the full file content.
 *
 * @param path       File path to read
 * @param buf        Output buffer
 * @param buf_size   Total buffer capacity
 * @param offset     Current write position (updated on success)
 * @return 0 on success, non-zero on error
 */
static int read_file_into(const char *path, char *buf, size_t buf_size, size_t *offset) {
   struct stat st;
   if (stat(path, &st) != 0) {
      return FAILURE; /* File not found — caller may treat as optional */
   }

   size_t file_size = (size_t)st.st_size;
   if (file_size == 0) {
      return SUCCESS;
   }

   /* Check if file fits in remaining buffer space */
   size_t remaining = buf_size - *offset - 1; /* -1 for null terminator */
   if (file_size > remaining) {
      OLOG_WARNING("Instruction file too large: %s (%zu bytes, %zu remaining)", path, file_size,
                   remaining);
      return FAILURE;
   }

   FILE *f = fopen(path, "r");
   if (!f) {
      return FAILURE;
   }

   size_t bytes_read = fread(buf + *offset, 1, file_size, f);
   fclose(f);

   *offset += bytes_read;
   return SUCCESS;
}

/**
 * @brief Append a separator between sections
 */
static void append_separator(char *buf, size_t buf_size, size_t *offset) {
   if (*offset + SEPARATOR_LEN < buf_size - 1) {
      /* append into a buffer the caller NUL-terminates */
      // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
      memcpy(buf + *offset, SEPARATOR, SEPARATOR_LEN);
      *offset += SEPARATOR_LEN;
   }
}

const char *instruction_loader_get_base_dir(void) {
   return INSTRUCTION_LOADER_DEFAULT_DIR;
}

int instruction_loader_load(const char *tool_name, const char *modules, char **output) {
   *output = NULL;

   if (!tool_name || tool_name[0] == '\0') {
      OLOG_ERROR("instruction_loader: tool_name is empty");
      return 1;
   }

   if (!is_safe_module_name(tool_name)) {
      OLOG_ERROR("instruction_loader: invalid tool_name '%s'", tool_name);
      return 1;
   }

   const char *base_dir = instruction_loader_get_base_dir();
   char path[1024];

   /* Pre-scan file sizes to allocate right-sized buffer instead of 128KB.
    * Start with _core.md, then each module. Add separator overhead. */
   size_t total_size = 0;
   struct stat st;

   snprintf(path, sizeof(path), "%s/%s/_core.md", base_dir, tool_name);
   if (stat(path, &st) == 0) {
      total_size += (size_t)st.st_size + SEPARATOR_LEN;
   }

   if (modules && modules[0] != '\0') {
      /* Quick scan — parse module names and stat each file */
      char *scan_copy = strdup(modules);
      if (scan_copy) {
         char *sp = NULL;
         char *m = strtok_r(scan_copy, ",", &sp);
         while (m) {
            while (*m == ' ')
               m++;
            size_t mlen = strlen(m);
            while (mlen > 0 && m[mlen - 1] == ' ')
               mlen--;
            if (mlen > 0) {
               m[mlen] = '\0';
               if (!is_safe_module_name(m)) {
                  m = strtok_r(NULL, ",", &sp);
                  continue;
               }
               char mpath[1024];
               snprintf(mpath, sizeof(mpath), "%s/%s/%s.md", base_dir, tool_name, m);
               if (stat(mpath, &st) == 0) {
                  total_size += (size_t)st.st_size + SEPARATOR_LEN;
               }
            }
            m = strtok_r(NULL, ",", &sp);
         }
         free(scan_copy);
      }
   }

   /* Allocate right-sized buffer (minimum 4KB, capped at 128KB) */
   if (total_size == 0) {
      total_size = 4096;
   } else {
      total_size += 256; /* Margin for separators and null terminator */
   }
   if (total_size > INSTRUCTION_LOADER_MAX_SIZE) {
      total_size = INSTRUCTION_LOADER_MAX_SIZE;
   }

   char *buf = (char *)malloc(total_size);
   if (!buf) {
      OLOG_ERROR("instruction_loader: failed to allocate %zu bytes", total_size);
      return 1;
   }

   size_t offset = 0;

   /* Always load _core.md first if it exists */
   snprintf(path, sizeof(path), "%s/%s/_core.md", base_dir, tool_name);
   if (read_file_into(path, buf, total_size, &offset) == 0 && offset > 0) {
      OLOG_INFO("instruction_loader: loaded _core.md (%zu bytes)", offset);
   }

   /* Parse and load each requested module */
   if (modules && modules[0] != '\0') {
      /* Make a mutable copy for strtok */
      char *modules_copy = strdup(modules);
      if (!modules_copy) {
         free(buf);
         return 1;
      }

      char *saveptr = NULL;
      char *module = strtok_r(modules_copy, ",", &saveptr);

      while (module != NULL) {
         /* Trim leading whitespace */
         while (*module == ' ') {
            module++;
         }

         /* Trim trailing whitespace */
         size_t len = strlen(module);
         while (len > 0 && module[len - 1] == ' ') {
            module[--len] = '\0';
         }

         if (len == 0) {
            module = strtok_r(NULL, ",", &saveptr);
            continue;
         }

         if (!is_safe_module_name(module)) {
            OLOG_WARNING("instruction_loader: skipping invalid module name '%s'", module);
            module = strtok_r(NULL, ",", &saveptr);
            continue;
         }

         /* Add separator before this module if we already have content */
         if (offset > 0) {
            append_separator(buf, total_size, &offset);
         }

         snprintf(path, sizeof(path), "%s/%s/%s.md", base_dir, tool_name, module);
         size_t before = offset;
         if (read_file_into(path, buf, total_size, &offset) == 0) {
            OLOG_INFO("instruction_loader: loaded %s.md (%zu bytes)", module, offset - before);
         } else {
            OLOG_WARNING("instruction_loader: module not found: %s/%s", tool_name, module);
            /* Remove the separator we added */
            offset = before > SEPARATOR_LEN ? before - SEPARATOR_LEN : before;
         }

         module = strtok_r(NULL, ",", &saveptr);
      }

      free(modules_copy);
   }

   /* Null-terminate */
   buf[offset] = '\0';

   if (offset == 0) {
      OLOG_WARNING("instruction_loader: no content loaded for tool '%s' modules '%s'", tool_name,
                   modules ? modules : "(none)");
      free(buf);
      return 1;
   }

   /* Shrink buffer to actual size */
   /* offset is bounded by the capped file size */
   // NOLINTNEXTLINE(clang-analyzer-optin.taint.TaintedAlloc)
   char *result = (char *)realloc(buf, offset + 1);
   *output = result ? result : buf;

   OLOG_INFO("instruction_loader: loaded %zu bytes total for %s", offset, tool_name);
   return 0;
}
