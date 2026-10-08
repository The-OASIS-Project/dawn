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
 * phone_audio_config — the [phone] call-audio settings block, parsed from and
 * written to dawn.toml as ONE unit so parse<->write stay symmetric.
 *
 * config_write_toml() truncates and rewrites the whole file; a tool section with
 * a parser but no matching writer is silently dropped ("saves but doesn't load").
 * Keeping the audio config's parse + write together — and unit-testing their
 * round-trip — turns that recurring bug class into a compile/CI-checkable
 * invariant.  Dependency-light on purpose (toml + phone_apm only) so the test
 * needs no phone_service/phone_db stubs.
 */

#ifndef PHONE_AUDIO_CONFIG_H
#define PHONE_AUDIO_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "audio/phone_apm.h"
#include "toml.h"

/* Downlink soft-limiter small-signal gain: valid range (0 = "use bridge default"). */
#define PHONE_DOWNLINK_GAIN_MAX 32.0f

/* pcm_port buffer size.  A raw /dev/ttyUSBn is tiny, but a udev stable alias
 * (/dev/serial/by-id/usb-...-if06-port0) runs ~90+ chars, so 64 truncates it. */
#define PHONE_PCM_PORT_MAX 128

/* Owned, TOML-parsed call-audio config. Kept separate from phone_tool_config_t
 * (the tool's behavioural params) — this is the audio/bridge tuning surface. */
typedef struct {
   char pcm_port[PHONE_PCM_PORT_MAX]; /* modem USB audio node; empty -> daemon default */
   phone_apm_config_t uplink;         /* near-end (mic) processing */
   phone_apm_config_t downlink;       /* far-end processing (used when downlink_use_apm) */
   bool downlink_use_apm;             /* true -> downlink AGC2 path; false -> soft-limiter */
   float downlink_gain;               /* soft-limiter gain (used when !downlink_use_apm) */
} phone_audio_config_t;

/** @brief Defaults (pcm_port empty, uplink = phone_apm defaults, downlink_gain 0
 *         → bridge default). */
phone_audio_config_t phone_audio_config_default(void);

/** @brief Overlay any keys present in @p table onto @p out (unset keys keep their
 *         current value — call with defaults first). Safe on a NULL table. */
void phone_audio_config_parse(const toml_table_t *table, phone_audio_config_t *out);

/** @brief Write every key phone_audio_config_parse() reads, at the current file
 *         position (inside an already-emitted [phone] section). */
void phone_audio_config_write(FILE *fp, const phone_audio_config_t *cfg);

/** @brief NS level <-> string mapping (shared by config I/O and the WebUI). */
const char *phone_ns_level_to_string(phone_ns_level_t lvl);
phone_ns_level_t phone_ns_level_from_string(const char *s);

/**
 * @brief Pure syntax check for an acceptable modem PCM serial node.
 *
 * Accepts a raw /dev/ttyUSB<n> / /dev/ttyACM<n> (1-3 trailing digits), or a
 * udev stable alias under /dev/serial/by-id/ or /dev/serial/by-path/ (a single
 * path component — no '/', no ".."). Does NOT touch the filesystem, so it is
 * safe to call on an untrusted WebUI value and is unit-testable without a
 * device present. Device identity is confirmed separately by
 * phone_pcm_port_resolve() + the bridge's post-open S_ISCHR fstat.
 *
 * @return true if @p path is syntactically an acceptable serial node.
 */
bool phone_pcm_port_validate(const char *path);

/**
 * @brief Validate + realpath @p path to the concrete /dev/ttyUSB<n> node.
 *
 * Runs phone_pcm_port_validate(), then realpath()s and requires the resolved
 * target to itself be a raw /dev/ttyUSB<n> / /dev/ttyACM<n> node. Callers open
 * @p resolved (never the alias), so open() always lands on a validated real
 * node and O_NOFOLLOW stays meaningful (the resolved node is not a symlink).
 * Touches the filesystem — hardware/integration path, not unit-tested.
 *
 * @param path          Configured port (raw node or stable alias).
 * @param resolved      Output buffer for the resolved real node path.
 * @param resolved_size Size of @p resolved.
 * @return true on success (@p resolved filled), false if invalid/unresolvable.
 */
bool phone_pcm_port_resolve(const char *path, char *resolved, size_t resolved_size);

#endif /* PHONE_AUDIO_CONFIG_H */
