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
 * Provider-neutral model-id version parsing, shared by the OpenAI and Claude
 * capability gates. Kept dependency-free (string/stdlib only) so it can be
 * unit-tested in isolation.
 */

#ifndef LLM_MODEL_VERSION_H
#define LLM_MODEL_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse the first major.minor version token out of a model id.
 *
 * Takes the first numeric token that follows a delimiter (string start, '-', or
 * '.'), e.g. "gpt-5.6-luna" -> 5.6, "gpt-5.4-mini" -> 5.4, "gpt-5" -> 5.0,
 * "claude-opus-4-8" -> 4.8, "claude-sonnet-5" -> 5.0, "claude-3-5-sonnet-20241022"
 * -> 3.5. The version is the first version token in current OpenAI/Claude ids, so a
 * trailing date or variant suffix does not interfere. A NULL/empty id yields 0.0.
 *
 * @param model_name Model id (may be NULL).
 * @param major_out  Receives the major version (0 if none found).
 * @param minor_out  Receives the minor version (0 if absent).
 */
void llm_parse_model_version(const char *model_name, int *major_out, int *minor_out);

#ifdef __cplusplus
}
#endif

#endif /* LLM_MODEL_VERSION_H */
