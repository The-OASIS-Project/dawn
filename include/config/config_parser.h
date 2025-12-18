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
 * DAWN Configuration Parser - TOML file parsing interface
 */

#ifndef CONFIG_PARSER_H
#define CONFIG_PARSER_H

#include "config/dawn_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse a TOML configuration file into a config struct
 *
 * Parses the specified TOML file and populates the config struct.
 * Fields not specified in the file retain their default values.
 *
 * @param path Path to the TOML config file
 * @param config Config struct to populate (should be pre-initialized with defaults)
 * @return 0 on success (SUCCESS), 1 on failure (FAILURE)
 */
int config_parse_file(const char *path, dawn_config_t *config);

/**
 * @brief Parse a secrets TOML file
 *
 * Parses the secrets file (API keys, credentials).
 *
 * @param path Path to the secrets TOML file
 * @param secrets Secrets struct to populate
 * @return 0 on success (SUCCESS), 1 on failure (FAILURE)
 */
int config_parse_secrets(const char *path, secrets_config_t *secrets);

/**
 * @brief Check if a configuration file exists and is readable
 *
 * Note: This function returns a boolean (true/false), NOT SUCCESS/FAILURE.
 * Use it in conditionals like: if (config_file_readable(path)) { ... }
 *
 * @param path Path to check
 * @return true (non-zero) if file exists and is readable, false (0) otherwise
 */
int config_file_readable(const char *path);

/**
 * @brief Find and load the configuration file
 *
 * Searches for config files in order:
 * 1. --config=PATH (if provided)
 * 2. ./dawn.toml
 * 3. ~/.config/dawn/config.toml
 * 4. /etc/dawn/config.toml
 *
 * @param explicit_path Explicit path from command line (NULL to use search)
 * @param config Config struct to populate
 * @return 0 on success, 1 if no config found (uses defaults)
 */
int config_load_from_search(const char *explicit_path, dawn_config_t *config);

/**
 * @brief Find and load secrets file
 *
 * Searches for secrets in order:
 * 1. ./secrets.toml (current directory)
 * 2. ~/.config/dawn/secrets.toml (user-specific)
 * 3. /etc/dawn/secrets.toml (system-wide)
 *
 * @param secrets Secrets struct to populate
 * @return 0 on success, 1 if not found (not an error, secrets are optional)
 */
int config_load_secrets_from_search(secrets_config_t *secrets);

/**
 * @brief Get the path to the loaded config file
 *
 * @return Path string, or "(none - using defaults)" if no file was loaded
 */
const char *config_get_loaded_path(void);

/**
 * @brief Get the path to the loaded secrets file
 *
 * @return Path string, or "(none)" if no secrets file was loaded
 */
const char *config_get_secrets_path(void);

/**
 * @brief Create a backup of a configuration file
 *
 * Creates a .bak copy of the specified file before modifications.
 * If the file doesn't exist, returns success (nothing to backup).
 *
 * @param path Path to the file to backup
 * @return 0 on success, 1 on failure
 */
int config_backup_file(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_PARSER_H */
