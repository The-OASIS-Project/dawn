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
 * Per-process scratch paths for tests, so test runs that overlap (two
 * worktrees, or a hook and a manual run) never share a file.
 */

#ifndef TEST_TMP_H
#define TEST_TMP_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/** Room for a scratch path: the temp directory plus a short name.  No larger than
 *  the 256-byte path fields a test copies it into (e.g. a config's db_path). */
#define TEST_TMP_PATH_MAX 256

/**
 * @brief Build "<temp dir>/<pid>-<name>" into @p buf.
 *
 * The temp directory is $TMPDIR, or /tmp when it's unset.  A fixed path such
 * as /tmp/dawn_test_x.db is shared by every run on the machine, so two runs at
 * once delete and lock each other's databases; the process id keeps them apart.
 *
 * @param buf  Output buffer (TEST_TMP_PATH_MAX is enough)
 * @param size Size of @p buf
 * @param name File name, e.g. "dawn_test_x.db"
 * @return @p buf
 */
static inline const char *test_tmp_path(char *buf, size_t size, const char *name) {
   const char *dir = getenv("TMPDIR");
   snprintf(buf, size, "%s/%d-%s", dir && dir[0] ? dir : "/tmp", (int)getpid(), name);
   return buf;
}

#endif /* TEST_TMP_H */
