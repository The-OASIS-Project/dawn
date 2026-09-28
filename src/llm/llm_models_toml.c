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
 * models.toml: the disk copy over the built-in one, table by table.
 */

#include "llm/llm_models_toml.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/config_parser.h"
#include "logging.h"
#include "models_toml_builtin.h" /* generated: MODELS_TOML_BUILTIN (cmake/embed_text.cmake) */
#include "tools/toml.h"

/* Locate models.toml via the config search path (cwd, then ~/.config/dawn, then
 * /etc/dawn — mirroring dawn.toml).  Returns true and fills `out` on the first hit. */
static bool find_models_toml_path(char *out, size_t out_sz) {
   if (config_file_readable("models.toml")) {
      snprintf(out, out_sz, "models.toml");
      return true;
   }
   const char *home = getenv("HOME");
   if (home && *home) {
      snprintf(out, out_sz, "%s/.config/dawn/models.toml", home);
      if (config_file_readable(out)) {
         return true;
      }
   }
   if (config_file_readable("/etc/dawn/models.toml")) {
      snprintf(out, out_sz, "/etc/dawn/models.toml");
      return true;
   }
   return false;
}

static toml_table_t *parse_builtin(void) {
   /* toml_parse takes a writable buffer. */
   char *copy = strdup(MODELS_TOML_BUILTIN);
   if (!copy) {
      return NULL;
   }
   char errbuf[200];
   toml_table_t *root = toml_parse(copy, errbuf, sizeof(errbuf));
   free(copy);
   if (!root) {
      OLOG_ERROR("models.toml (built-in) parse error: %s", errbuf);
   }
   return root;
}

static toml_table_t *parse_disk(const char *path) {
   FILE *fp = fopen(path, "r");
   if (!fp) {
      OLOG_WARNING("models.toml: cannot open %s (%s); using the built-in copy", path,
                   strerror(errno));
      return NULL;
   }
   char errbuf[200];
   toml_table_t *root = toml_parse_file(fp, errbuf, sizeof(errbuf));
   fclose(fp);
   if (!root) {
      OLOG_WARNING("models.toml: parse error in %s: %s; using the built-in copy", path, errbuf);
   }
   return root;
}

void llm_models_toml_open_file(const char *path, llm_models_toml_t *m) {
   memset(m, 0, sizeof(*m));
   if (path) {
      snprintf(m->path, sizeof(m->path), "%s", path);
      m->disk = parse_disk(path);
   }
   m->builtin = parse_builtin();
}

void llm_models_toml_open(llm_models_toml_t *m) {
   char path[512];
   llm_models_toml_open_file(find_models_toml_path(path, sizeof(path)) ? path : NULL, m);
   if (!m->path[0]) {
      OLOG_INFO("models.toml: none on disk; using the built-in copy");
   }
}

toml_table_t *llm_models_toml_root_for(const llm_models_toml_t *m, const char *table) {
   if (m->disk && toml_table_in(m->disk, table)) {
      return m->disk;
   }
   if (m->builtin && toml_table_in(m->builtin, table)) {
      if (m->disk) {
         OLOG_WARNING("models.toml: %s has no [%s] table (an older copy?); using DAWN's built-in "
                      "one. Merge the shipped models.toml to customize it.",
                      m->path, table);
      }
      return m->builtin;
   }
   return NULL;
}

void llm_models_toml_close(llm_models_toml_t *m) {
   if (m->disk) {
      toml_free(m->disk);
   }
   if (m->builtin) {
      toml_free(m->builtin);
   }
   memset(m, 0, sizeof(*m));
}
