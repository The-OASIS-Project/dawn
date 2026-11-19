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
 * DAWN Audio Backend - PulseAudio Implementation
 *
 * Provides PulseAudio-based audio capture and playback.
 *
 * Memory Management:
 *   - Uses static handle pools (no malloc/free per stream)
 *   - Maximum concurrent streams: PULSE_MAX_CAPTURE_HANDLES + PULSE_MAX_PLAYBACK_HANDLES
 *
 * Streams (pulse_stream.c):
 *   - Blocking read/write, as pa_simple, one mainloop per stream
 *   - A stream on a named device stays on it: when the device goes away, reads
 *     and writes fail with AUDIO_ERR_NO_DEVICE and the caller reopens it by name
 *     (pa_simple would move it to the default without saying so).  "default"
 *     follows the default device.
 *
 * Behavioral Notes:
 *   - avail() returns an estimate based on timing (not exact like ALSA)
 *   - read/write may block until full buffer is available
 *   - This is acceptable for DAWN's voice pipeline
 */

#include <pthread.h>
#include <pulse/error.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "audio/audio_backend.h"
#include "audio/pulse_stream.h"
#include "dawn_error.h"
#include "logging.h"

/* =============================================================================
 * Configuration
 * ============================================================================= */

#define PULSE_MAX_CAPTURE_HANDLES 2
#define PULSE_MAX_PLAYBACK_HANDLES 6
#define PULSE_HANDLE_MAGIC 0x50554C53 /* "PULS" in ASCII */

/* =============================================================================
 * Internal handle structures (with backend identifier for validation)
 * ============================================================================= */

struct audio_stream_capture_handle {
   uint32_t magic; /* PULSE_HANDLE_MAGIC for validation */
   bool in_use;    /* Handle allocation flag */
   pulse_stream_t *pa;
   audio_hw_params_t hw_params;
   size_t bytes_per_frame;
   uint64_t last_read_time_us; /* For avail() estimation */
};

struct audio_stream_playback_handle {
   uint32_t magic; /* PULSE_HANDLE_MAGIC for validation */
   bool in_use;    /* Handle allocation flag */
   pulse_stream_t *pa;
   audio_hw_params_t hw_params;
   size_t bytes_per_frame;
   bool drain_on_close; /* Whether to drain on close */
};

/* =============================================================================
 * Static handle pools (no malloc/free)
 * ============================================================================= */

static struct audio_stream_capture_handle g_capture_handles[PULSE_MAX_CAPTURE_HANDLES];
static struct audio_stream_playback_handle g_playback_handles[PULSE_MAX_PLAYBACK_HANDLES];
static pthread_mutex_t g_handle_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * Handle validation
 * ============================================================================= */

static bool validate_capture_handle(audio_stream_capture_handle_t *handle) {
   if (!handle || handle->magic != PULSE_HANDLE_MAGIC || !handle->in_use) {
      return false;
   }
   return true;
}

static bool validate_playback_handle(audio_stream_playback_handle_t *handle) {
   if (!handle || handle->magic != PULSE_HANDLE_MAGIC || !handle->in_use) {
      return false;
   }
   return true;
}

/* =============================================================================
 * Handle allocation
 * ============================================================================= */

static audio_stream_capture_handle_t *alloc_capture_handle(void) {
   audio_stream_capture_handle_t *handle = NULL;

   pthread_mutex_lock(&g_handle_mutex);
   for (int i = 0; i < PULSE_MAX_CAPTURE_HANDLES; i++) {
      if (!g_capture_handles[i].in_use) {
         memset(&g_capture_handles[i], 0, sizeof(g_capture_handles[i]));
         g_capture_handles[i].magic = PULSE_HANDLE_MAGIC;
         g_capture_handles[i].in_use = true;
         handle = &g_capture_handles[i];
         break;
      }
   }
   pthread_mutex_unlock(&g_handle_mutex);

   return handle;
}

static void free_capture_handle(audio_stream_capture_handle_t *handle) {
   if (!handle) {
      return;
   }

   pthread_mutex_lock(&g_handle_mutex);
   handle->magic = 0;
   handle->in_use = false;
   handle->pa = NULL;
   pthread_mutex_unlock(&g_handle_mutex);
}

static audio_stream_playback_handle_t *alloc_playback_handle(void) {
   audio_stream_playback_handle_t *handle = NULL;

   pthread_mutex_lock(&g_handle_mutex);
   for (int i = 0; i < PULSE_MAX_PLAYBACK_HANDLES; i++) {
      if (!g_playback_handles[i].in_use) {
         memset(&g_playback_handles[i], 0, sizeof(g_playback_handles[i]));
         g_playback_handles[i].magic = PULSE_HANDLE_MAGIC;
         g_playback_handles[i].in_use = true;
         g_playback_handles[i].drain_on_close = true; /* Default: drain */
         handle = &g_playback_handles[i];
         break;
      }
   }
   pthread_mutex_unlock(&g_handle_mutex);

   return handle;
}

static void free_playback_handle(audio_stream_playback_handle_t *handle) {
   if (!handle) {
      return;
   }

   pthread_mutex_lock(&g_handle_mutex);
   handle->magic = 0;
   handle->in_use = false;
   handle->pa = NULL;
   pthread_mutex_unlock(&g_handle_mutex);
}

/* =============================================================================
 * Helper functions
 * ============================================================================= */

/**
 * @brief Convert audio_sample_format_t to PulseAudio format
 */
static pa_sample_format_t format_to_pulse(audio_sample_format_t format) {
   switch (format) {
      case AUDIO_FORMAT_S16_LE:
         return PA_SAMPLE_S16LE;
      case AUDIO_FORMAT_S24_3LE:
         return PA_SAMPLE_S24LE;
      case AUDIO_FORMAT_S32_LE:
         return PA_SAMPLE_S32LE;
      case AUDIO_FORMAT_FLOAT32:
         return PA_SAMPLE_FLOAT32LE;
      default:
         return PA_SAMPLE_S16LE;
   }
}

/**
 * @brief Get current time in microseconds
 */
static uint64_t get_time_us(void) {
   struct timeval tv;
   gettimeofday(&tv, NULL);
   return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/** A stream error as an audio error: the device gone (or never there) is
 *  AUDIO_ERR_NO_DEVICE, so its caller knows to reopen it. */
static int pulse_error_to_audio(int error) {
   switch (error) {
      case PA_ERR_KILLED:
      case PA_ERR_NOENTITY:
         return AUDIO_ERR_NO_DEVICE;
      case PULSE_STREAM_ERR_SUSPENDED:
         return AUDIO_ERR_SUSPENDED;
      default:
         return AUDIO_ERR_IO;
   }
}

/* A source or sink named by its index ("3") gets a new index when its card is
 * plugged in again, so the stream can't find it after that.  Said once. */
static void pulse_warn_index_name(const char *device) {
   static bool warned = false;
   if (warned || !device || !*device) {
      return;
   }
   for (const char *p = device; *p; p++) {
      if (*p < '0' || *p > '9') {
         return;
      }
   }
   OLOG_WARNING("PulseAudio: device '%s' is an index, which changes when a USB device is "
                "plugged in again; use its name (pactl list sources/sinks short)",
                device);
   warned = true;
}

static const char *pulse_error_name(int error) {
   const char *name = error == PULSE_STREAM_ERR_SUSPENDED ? "suspended" : pa_strerror(error);
   return name ? name : "unknown error";
}

/* =============================================================================
 * PulseAudio Capture Implementation
 * ============================================================================= */

audio_stream_capture_handle_t *pulse_capture_open(const char *device,
                                                  const audio_stream_params_t *params,
                                                  audio_hw_params_t *hw_params) {
   int error = 0;

   audio_stream_capture_handle_t *handle = alloc_capture_handle();
   if (!handle) {
      OLOG_ERROR("PulseAudio: No free capture handles available");
      return NULL;
   }

   pa_sample_spec ss = { .format = format_to_pulse(params->format),
                         .rate = params->sample_rate,
                         .channels = (uint8_t)params->channels };

   /* Calculate bytes per frame first */
   size_t bpf = audio_bytes_per_frame(params->format, params->channels);

   /* Buffer attributes for low latency */
   pa_buffer_attr attr = { .maxlength = (uint32_t)-1,
                           .tlength = (uint32_t)-1,
                           .prebuf = (uint32_t)-1,
                           .minreq = (uint32_t)-1,
                           .fragsize = (uint32_t)(params->period_frames * bpf) };

   /* Use NULL for default device if device is "default" or NULL */
   const char *pa_device = NULL;
   if (device && strcmp(device, "default") != 0) {
      pa_device = device;
   }

   OLOG_DEBUG("PulseAudio: Opening capture device: %s", pa_device ? pa_device : "(default)");
   pulse_warn_index_name(pa_device);

   handle->pa = pulse_stream_new(PA_STREAM_RECORD, pa_device, "Audio Capture", &ss, &attr, &error);
   if (!handle->pa) {
      /* A missing device is its caller's to report (it may be waiting for it). */
      if (error == PA_ERR_NOENTITY) {
         OLOG_DEBUG("PulseAudio: capture device %s not available",
                    pa_device ? pa_device : "(default)");
      } else {
         OLOG_ERROR("PulseAudio: Cannot open capture: %s", pulse_error_name(error));
      }
      free_capture_handle(handle);
      return NULL;
   }

   /* Store hardware parameters (PulseAudio accepts what we ask for) */
   handle->hw_params.sample_rate = params->sample_rate;
   handle->hw_params.channels = params->channels;
   handle->hw_params.format = params->format;
   handle->hw_params.period_frames = params->period_frames;
   handle->hw_params.buffer_frames = params->buffer_frames;
   handle->bytes_per_frame = bpf;
   handle->last_read_time_us = get_time_us();

   if (hw_params) {
      *hw_params = handle->hw_params;
   }

   OLOG_INFO("PulseAudio capture: %s rate=%u ch=%u format=%d", pa_device ? pa_device : "(default)",
             handle->hw_params.sample_rate, handle->hw_params.channels, handle->hw_params.format);

   return handle;
}

ssize_t pulse_capture_read(audio_stream_capture_handle_t *handle, void *buffer, size_t frames) {
   if (!validate_capture_handle(handle) || !buffer) {
      return -AUDIO_ERR_INVALID;
   }

   int error = 0;
   size_t bytes = frames * handle->bytes_per_frame;

   if (pulse_stream_read(handle->pa, buffer, bytes, &error) != SUCCESS) {
      if (error != PULSE_STREAM_ERR_SUSPENDED) { /* the caller says that once */
         OLOG_DEBUG("PulseAudio capture read failed: %s", pulse_error_name(error));
      }
      return -pulse_error_to_audio(error);
   }

   /* Update timing for avail() estimation */
   handle->last_read_time_us = get_time_us();

   return (ssize_t)frames;
}

ssize_t pulse_capture_avail(audio_stream_capture_handle_t *handle) {
   if (!validate_capture_handle(handle)) {
      return -AUDIO_ERR_INVALID;
   }

   /*
    * The stream gives no avail query here.
    * Estimate based on time since last read and sample rate.
    * This is approximate but sufficient for flow control decisions.
    */
   uint64_t now = get_time_us();
   uint64_t elapsed_us = now - handle->last_read_time_us;

   /* Frames accumulated = elapsed_time * sample_rate / 1000000 */
   size_t accumulated = (size_t)(elapsed_us * handle->hw_params.sample_rate / 1000000);

   /* Cap at buffer size */
   if (accumulated > handle->hw_params.buffer_frames) {
      accumulated = handle->hw_params.buffer_frames;
   }

   return (ssize_t)accumulated;
}

int pulse_capture_recover(audio_stream_capture_handle_t *handle, int err) {
   if (!validate_capture_handle(handle)) {
      return AUDIO_ERR_INVALID;
   }

   /* Nothing to recover in place: a failed stream stays failed (reopen it) */
   /* Just reset timing state */
   handle->last_read_time_us = get_time_us();

   (void)err;
   return AUDIO_SUCCESS;
}

void pulse_capture_close(audio_stream_capture_handle_t *handle) {
   if (!validate_capture_handle(handle)) {
      return;
   }

   if (handle->pa) {
      pulse_stream_free(handle->pa);
   }

   free_capture_handle(handle);
   OLOG_INFO("PulseAudio capture closed");
}

/* =============================================================================
 * PulseAudio Playback Implementation
 * ============================================================================= */

audio_stream_playback_handle_t *pulse_playback_open(const char *device,
                                                    const audio_stream_params_t *params,
                                                    audio_hw_params_t *hw_params) {
   int error = 0;

   audio_stream_playback_handle_t *handle = alloc_playback_handle();
   if (!handle) {
      OLOG_ERROR("PulseAudio: No free playback handles available");
      return NULL;
   }

   pa_sample_spec ss = { .format = format_to_pulse(params->format),
                         .rate = params->sample_rate,
                         .channels = (uint8_t)params->channels };

   /* Calculate bytes per frame first */
   size_t bpf = audio_bytes_per_frame(params->format, params->channels);

   /* Buffer attributes */
   pa_buffer_attr attr = { .maxlength = (uint32_t)-1,
                           .tlength = (uint32_t)(params->buffer_frames * bpf),
                           .prebuf = (uint32_t)-1,
                           .minreq = (uint32_t)(params->period_frames * bpf),
                           .fragsize = (uint32_t)-1 };

   /* Use NULL for default device if device is "default" or NULL */
   const char *pa_device = NULL;
   if (device && strcmp(device, "default") != 0) {
      pa_device = device;
   }

   OLOG_DEBUG("PulseAudio: Opening playback device: %s", pa_device ? pa_device : "(default)");
   pulse_warn_index_name(pa_device);

   handle->pa = pulse_stream_new(PA_STREAM_PLAYBACK, pa_device, "Audio Playback", &ss, &attr,
                                 &error);
   if (!handle->pa) {
      /* A missing device is its caller's to report (it may be waiting for it). */
      if (error == PA_ERR_NOENTITY) {
         OLOG_DEBUG("PulseAudio: playback device %s not available",
                    pa_device ? pa_device : "(default)");
      } else {
         OLOG_ERROR("PulseAudio: Cannot open playback: %s", pulse_error_name(error));
      }
      free_playback_handle(handle);
      return NULL;
   }

   /* Store hardware parameters */
   handle->hw_params.sample_rate = params->sample_rate;
   handle->hw_params.channels = params->channels;
   handle->hw_params.format = params->format;
   handle->hw_params.period_frames = params->period_frames;
   handle->hw_params.buffer_frames = params->buffer_frames;
   handle->bytes_per_frame = bpf;
   handle->drain_on_close = true;

   if (hw_params) {
      *hw_params = handle->hw_params;
   }

   OLOG_INFO("PulseAudio playback: %s rate=%u ch=%u format=%d", pa_device ? pa_device : "(default)",
             handle->hw_params.sample_rate, handle->hw_params.channels, handle->hw_params.format);

   return handle;
}

ssize_t pulse_playback_write(audio_stream_playback_handle_t *handle,
                             const void *buffer,
                             size_t frames) {
   if (!validate_playback_handle(handle) || !buffer) {
      return -AUDIO_ERR_INVALID;
   }

   int error = 0;
   size_t bytes = frames * handle->bytes_per_frame;

   if (pulse_stream_write(handle->pa, buffer, bytes, &error) != SUCCESS) {
      OLOG_DEBUG("PulseAudio playback write failed: %s", pulse_error_name(error));
      return -pulse_error_to_audio(error);
   }

   return (ssize_t)frames;
}

ssize_t pulse_playback_avail(audio_stream_playback_handle_t *handle) {
   if (!validate_playback_handle(handle)) {
      return -AUDIO_ERR_INVALID;
   }

   /*
    * The stream gives no avail query here.
    * Return the buffer size as an estimate - this indicates
    * how much can potentially be written without blocking.
    *
    * For DAWN's use case, writes are typically small (one period)
    * and blocking is acceptable.
    */
   return (ssize_t)handle->hw_params.buffer_frames;
}

int pulse_playback_drain(audio_stream_playback_handle_t *handle) {
   if (!validate_playback_handle(handle)) {
      return AUDIO_ERR_INVALID;
   }

   int error = 0;
   if (pulse_stream_drain(handle->pa, &error) != SUCCESS) {
      OLOG_DEBUG("PulseAudio playback drain failed: %s", pulse_error_name(error));
      return pulse_error_to_audio(error);
   }

   return AUDIO_SUCCESS;
}

int pulse_playback_drop(audio_stream_playback_handle_t *handle) {
   if (!validate_playback_handle(handle)) {
      return AUDIO_ERR_INVALID;
   }

   int error = 0;
   if (pulse_stream_flush(handle->pa, &error) != SUCCESS) {
      OLOG_DEBUG("PulseAudio playback flush failed: %s", pulse_error_name(error));
      return pulse_error_to_audio(error);
   }

   /* Don't drain on close after explicit drop */
   handle->drain_on_close = false;

   return AUDIO_SUCCESS;
}

int pulse_playback_recover(audio_stream_playback_handle_t *handle, int err) {
   if (!validate_playback_handle(handle)) {
      return AUDIO_ERR_INVALID;
   }

   /* Nothing to recover in place: a failed stream stays failed (reopen it) */
   (void)err;
   return AUDIO_SUCCESS;
}

void pulse_playback_close(audio_stream_playback_handle_t *handle) {
   if (!validate_playback_handle(handle)) {
      return;
   }

   if (handle->pa) {
      /* Only drain if not previously dropped */
      if (handle->drain_on_close) {
         int error = 0;
         if (pulse_stream_drain(handle->pa, &error) != SUCCESS) {
            OLOG_DEBUG("PulseAudio playback: drain on close failed: %s", pulse_error_name(error));
         }
      }
      pulse_stream_free(handle->pa);
   }

   free_playback_handle(handle);
   OLOG_INFO("PulseAudio playback closed");
}
