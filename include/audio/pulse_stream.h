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
 * A blocking PulseAudio stream, like pa_simple, that stays on the device it
 * was opened on: when that device goes away the stream fails instead of being
 * moved to the default.
 */

#ifndef PULSE_STREAM_H
#define PULSE_STREAM_H

#include <pulse/def.h>
#include <pulse/sample.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pulse_stream pulse_stream_t;

/** Read error: the device is suspended (no PA_ERR_* says so). */
#define PULSE_STREAM_ERR_SUSPENDED (PA_ERR_MAX + 1)

/**
 * @brief Open a record or playback stream, blocking until it is ready
 *
 * @param device  A sink or source name; NULL, "default" or a name starting
 *                with '@' follows the default device (and may be moved)
 * @param error   PulseAudio error code (PA_ERR_*) on failure; PA_ERR_NOENTITY
 *                when the named device isn't there, or the stream was linked to
 *                another one
 * @return The stream, or NULL
 */
pulse_stream_t *pulse_stream_new(pa_stream_direction_t dir,
                                 const char *device,
                                 const char *description,
                                 const pa_sample_spec *ss,
                                 const pa_buffer_attr *attr,
                                 int *error);

/** Read exactly @p bytes (a hole reads as silence); SUCCESS, or FAILURE with
 *  @p error set (PULSE_STREAM_ERR_SUSPENDED while the source is suspended). */
int pulse_stream_read(pulse_stream_t *s, void *data, size_t bytes, int *error);

/** Write all @p bytes; SUCCESS, or FAILURE with @p error set. */
int pulse_stream_write(pulse_stream_t *s, const void *data, size_t bytes, int *error);

/** Wait until what was written has played; SUCCESS or FAILURE. */
int pulse_stream_drain(pulse_stream_t *s, int *error);

/** Discard what was written and not yet played; SUCCESS or FAILURE. */
int pulse_stream_flush(pulse_stream_t *s, int *error);

/** Close the stream (NULL is ignored). */
void pulse_stream_free(pulse_stream_t *s);

#ifdef __cplusplus
}
#endif

#endif /* PULSE_STREAM_H */
