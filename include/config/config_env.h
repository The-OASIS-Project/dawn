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
 * DAWN Configuration Environment - Environment variable overrides
 */

#ifndef CONFIG_ENV_H
#define CONFIG_ENV_H

#include "config/dawn_config.h"

/* Forward declaration for json-c */
struct json_object;
typedef struct json_object json_object;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Apply environment variable overrides to configuration
 *
 * Reads DAWN_* environment variables and applies them to the config.
 * Also reads standard API key environment variables (OPENAI_API_KEY, etc.)
 *
 * Environment variable format: DAWN_<SECTION>_<KEY>
 * Examples:
 *   DAWN_AUDIO_BACKEND=alsa
 *   DAWN_VAD_SPEECH_THRESHOLD=0.6
 *   DAWN_LLM_TYPE=local
 *   DAWN_LLM_CLOUD_PROVIDER=claude
 *
 * Standard API keys (higher priority for secrets):
 *   OPENAI_API_KEY -> secrets.openai_api_key
 *   ANTHROPIC_API_KEY -> secrets.claude_api_key
 *
 * @param config Config struct to modify
 * @param secrets Secrets struct to modify
 */
void config_apply_env(dawn_config_t *config, secrets_config_t *secrets);

/**
 * @brief Dump configuration to stdout
 *
 * Prints all configuration values in a readable format.
 * Used by --dump-config CLI option.
 *
 * @param config Configuration to dump
 */
void config_dump(const dawn_config_t *config);

/**
 * @brief Dump configuration as TOML
 *
 * Prints configuration in TOML format that can be saved to a file.
 *
 * @param config Configuration to dump
 */
void config_dump_toml(const dawn_config_t *config);

/**
 * @brief Dump all settings with sources and environment variable names
 *
 * Prints each configuration setting with:
 * - Setting name (TOML path)
 * - Current value
 * - Environment variable name
 * - Inferred source (default, file, env)
 *
 * Used by --dump-settings CLI option.
 *
 * @param config Configuration to dump
 * @param secrets Secrets to check (for API key env vars)
 * @param config_file_loaded Path to loaded config file (NULL if none)
 */
void config_dump_settings(const dawn_config_t *config,
                          const secrets_config_t *secrets,
                          const char *config_file_loaded);

/**
 * @brief Convert configuration to JSON object for WebUI
 *
 * Serializes the entire config structure to a json-c object.
 * Caller must free the returned object with json_object_put().
 *
 * @param config Configuration to serialize
 * @return json_object* JSON object (caller owns), or NULL on error
 */
json_object *config_to_json(const dawn_config_t *config);

/**
 * @brief Get secrets status as JSON (without revealing values)
 *
 * Returns a JSON object with boolean flags indicating which secrets are set.
 * Never includes actual secret values.
 *
 * @param secrets Secrets to check
 * @return json_object* JSON object with is_set flags, or NULL on error
 */
json_object *secrets_to_json_status(const secrets_config_t *secrets);

/**
 * @brief Write configuration to TOML file
 *
 * Writes the complete configuration to the specified path in TOML format.
 *
 * @param config Configuration to write
 * @param path Output file path
 * @return 0 on success, non-zero on error
 */
int config_write_toml(const dawn_config_t *config, const char *path);

/**
 * @brief Write secrets to TOML file
 *
 * Writes secrets to the specified path. Sets restrictive file permissions (0600).
 *
 * @param secrets Secrets to write
 * @param path Output file path
 * @return 0 on success, non-zero on error
 */
int secrets_write_toml(const secrets_config_t *secrets, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_ENV_H */
