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
 * models.toml, the read-only model reference data (context windows, cache
 * pricing, reasoning capabilities): the copy on disk, and the shipped copy
 * compiled into DAWN.  Each table comes from the disk copy when it has it and
 * from the built-in copy otherwise, so an older models.toml (a service install
 * keeps its own) can't silently drop a table a newer DAWN depends on.
 */

#ifndef LLM_MODELS_TOML_H
#define LLM_MODELS_TOML_H

#ifdef __cplusplus
extern "C" {
#endif

struct toml_table_t;

/** The parsed disk and built-in copies. */
typedef struct {
   struct toml_table_t *disk;    /**< NULL: no file, or it didn't parse */
   struct toml_table_t *builtin; /**< The shipped copy */
   char path[512];               /**< The disk copy's path ("" if none) */
} llm_models_toml_t;

/**
 * @brief Parse models.toml from the config search path, plus the built-in copy
 *
 * The search path mirrors dawn.toml's: ./models.toml, ~/.config/dawn/, then
 * /etc/dawn/.  Close with llm_models_toml_close().
 */
void llm_models_toml_open(llm_models_toml_t *m);

/**
 * @brief Parse a given models.toml, plus the built-in copy
 *
 * @param path The disk copy (NULL: built-in only)
 */
void llm_models_toml_open_file(const char *path, llm_models_toml_t *m);

/**
 * @brief The root to read top-level table @p table from
 *
 * The disk copy's if it has the table, else the built-in copy's (logged, with
 * the disk file's path, when a disk copy exists without it).
 *
 * @return A root holding @p table, or NULL if neither copy has it
 */
struct toml_table_t *llm_models_toml_root_for(const llm_models_toml_t *m, const char *table);

/** Free both copies. */
void llm_models_toml_close(llm_models_toml_t *m);

#ifdef __cplusplus
}
#endif

#endif /* LLM_MODELS_TOML_H */
