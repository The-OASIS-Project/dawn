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
 * Request origins for the same-origin (CSRF) check: pure string handling.
 */

#ifndef WEBUI_ORIGIN_H
#define WEBUI_ORIGIN_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The origin of a browser Referer: "scheme://authority", the
 *        authority ending at the first '/', '?' or '#'.
 *
 * Compared exactly afterwards, never by prefix: the origin of
 * "https://ours.example.evil.com/" is not "https://ours.example".
 *
 * @return false for a Referer with no "://", an empty authority, userinfo
 *         ('@', which no page of ours sends), or one too long for @p out.
 */
bool webui_referer_origin(const char *referer, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_ORIGIN_H */
