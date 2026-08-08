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
 * Single source of truth for the Tier 1 satellite firmware version.  Reported
 * to the daemon at registration (firmware_version) for OTA fleet visibility.
 * Bump this on every release that ships a new satellite binary.
 */

#ifndef SATELLITE_VERSION_H
#define SATELLITE_VERSION_H

#define DAWN_SATELLITE_FIRMWARE_VERSION "2.3.0"

/* Greppable, stable marker embedded in the compiled binary (see the `used` global
 * in main.c) so the OTA release/sign tooling can read the EXACT version baked into
 * the image being signed.  A plain header read can pass while the binary is a stale
 * build; reading this marker out of the binary cannot.  Format is intentionally a
 * single `KEY=value` token so `strings <binary> | grep` recovers the value cleanly. */
#define DAWN_SAT_FW_VERSION_MARKER "DAWN_SAT_FW_VERSION=" DAWN_SATELLITE_FIRMWARE_VERSION

#endif /* SATELLITE_VERSION_H */
