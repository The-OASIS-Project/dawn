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
 * The commit this build is from (a short hash, or "unknown").  Defined in a
 * file CMake generates (cmake/git_sha.c.in), so a new commit recompiles that
 * one file rather than everything a -D would reach.
 */

#ifndef GIT_SHA_H
#define GIT_SHA_H

#ifdef __cplusplus
extern "C" {
#endif

extern const char dawn_git_sha[];

#ifdef __cplusplus
}
#endif

#endif /* GIT_SHA_H */
