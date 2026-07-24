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
 * Warning-counter hooks exposed by test_config_roundtrip_stub.c.
 */

#ifndef TEST_CONFIG_ROUNDTRIP_STUB_H
#define TEST_CONFIG_ROUNDTRIP_STUB_H

/** Zero the counter (call before the operation under test). */
void test_stub_reset_warnings(void);

/** @return WARNING+ERROR log lines emitted since the last reset. */
int test_stub_warning_count(void);

#endif /* TEST_CONFIG_ROUNDTRIP_STUB_H */
