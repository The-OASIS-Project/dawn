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
 * DAWN Audio Backend - ALSA Implementation
 *
 * Provides ALSA-based audio capture and playback with low-latency operation
 * suitable for embedded systems.
 *
 * Memory Management:
 *   - Uses static handle pools (no malloc/free per stream)
 *   - Maximum concurrent streams: ALSA_MAX_CAPTURE_HANDLES + ALSA_MAX_PLAYBACK_HANDLES
 *
 * Error Handling:
 *   - Maps ALSA error codes to AUDIO_ERR_* values
 *   - Supports recovery from EPIPE (xrun) and ESTRPIPE (suspend)
 */

#include <alsa/asoundlib.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "audio/audio_backend.h"
#include "logging.h"

/* =============================================================================
 * Configuration
 * ============================================================================= */

#define ALSA_MAX_CAPTURE_HANDLES 2
#define ALSA_MAX_PLAYBACK_HANDLES 6
#define ALSA_HANDLE_MAGIC 0x414C5341 /* "ALSA" in ASCII */

/* =============================================================================
 * Internal handle structures (with backend identifier for validation)
 * ============================================================================= */

struct audio_stream_capture_handle {
   uint32_t magic; /* ALSA_HANDLE_MAGIC for validation */
   bool in_use;    /* Handle allocation flag */
   snd_pcm_t *pcm; /* ALSA PCM handle */
   audio_hw_params_t hw_params;
   size_t bytes_per_frame;
};

struct audio_stream_playback_handle {
   uint32_t magic; /* ALSA_HANDLE_MAGIC for validation */
   bool in_use;    /* Handle allocation flag */
   snd_pcm_t *pcm; /* ALSA PCM handle */
   audio_hw_params_t hw_params;
   size_t bytes_per_frame;
   bool drain_on_close; /* Whether to drain on close (false after drop) */
};

/* =============================================================================
 * Static handle pools (no malloc/free)
 * ============================================================================= */

static struct audio_stream_capture_handle g_capture_handles[ALSA_MAX_CAPTURE_HANDLES];
static struct audio_stream_playback_handle g_playback_handles[ALSA_MAX_PLAYBACK_HANDLES];
static pthread_mutex_t g_handle_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * Error code mapping
 * ============================================================================= */

/**
 * @brief Map ALSA error code to AUDIO_ERR_* value
 */
static int alsa_error_to_audio_error(int alsa_err) {
   if (alsa_err >= 0) {
      return AUDIO_SUCCESS;
   }

   switch (alsa_err) {
      case -EPIPE:
         return AUDIO_ERR_OVERRUN; /* or UNDERRUN depending on context */
      case -ESTRPIPE:
         return AUDIO_ERR_SUSPENDED;
      case -ENODEV:
      case -ENOENT:
         return AUDIO_ERR_NO_DEVICE;
      case -EBUSY:
         return AUDIO_ERR_BUSY;
      case -EINVAL:
         return AUDIO_ERR_INVALID;
      case -ETIMEDOUT:
         return AUDIO_ERR_TIMEOUT;
      case -EIO:
         return AUDIO_ERR_IO;
      default:
         return AUDIO_ERR_UNKNOWN;
   }
}

/* =============================================================================
 * Handle validation
 * ============================================================================= */

static bool validate_capture_handle(audio_stream_capture_handle_t *handle) {
   if (!handle || handle->magic != ALSA_HANDLE_MAGIC || !handle->in_use) {
      return false;
   }
   return true;
}

static bool validate_playback_handle(audio_stream_playback_handle_t *handle) {
   if (!handle || handle->magic != ALSA_HANDLE_MAGIC || !handle->in_use) {
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
   for (int i = 0; i < ALSA_MAX_CAPTURE_HANDLES; i++) {
      if (!g_capture_handles[i].in_use) {
         memset(&g_capture_handles[i], 0, sizeof(g_capture_handles[i]));
         g_capture_handles[i].magic = ALSA_HANDLE_MAGIC;
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
   handle->pcm = NULL;
   pthread_mutex_unlock(&g_handle_mutex);
}

static audio_stream_playback_handle_t *alloc_playback_handle(void) {
   audio_stream_playback_handle_t *handle = NULL;

   pthread_mutex_lock(&g_handle_mutex);
   for (int i = 0; i < ALSA_MAX_PLAYBACK_HANDLES; i++) {
      if (!g_playback_handles[i].in_use) {
         memset(&g_playback_handles[i], 0, sizeof(g_playback_handles[i]));
         g_playback_handles[i].magic = ALSA_HANDLE_MAGIC;
         g_playback_handles[i].in_use = true;
         g_playback_handles[i].drain_on_close = true; /* Default: drain on close */
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
   handle->pcm = NULL;
   pthread_mutex_unlock(&g_handle_mutex);
}

/* =============================================================================
 * Helper functions
 * ============================================================================= */

/**
 * @brief Convert audio_sample_format_t to ALSA format
 */
static snd_pcm_format_t format_to_alsa(audio_sample_format_t format) {
   switch (format) {
      case AUDIO_FORMAT_S16_LE:
         return SND_PCM_FORMAT_S16_LE;
      case AUDIO_FORMAT_S24_3LE:
         return SND_PCM_FORMAT_S24_3LE;
      case AUDIO_FORMAT_S32_LE:
         return SND_PCM_FORMAT_S32_LE;
      case AUDIO_FORMAT_FLOAT32:
         return SND_PCM_FORMAT_FLOAT_LE;
      default:
         return SND_PCM_FORMAT_S16_LE;
   }
}

/**
 * @brief Convert ALSA format to audio_sample_format_t
 */
static audio_sample_format_t alsa_to_format(snd_pcm_format_t alsa_format) {
   switch (alsa_format) {
      case SND_PCM_FORMAT_S16_LE:
         return AUDIO_FORMAT_S16_LE;
      case SND_PCM_FORMAT_S24_3LE:
         return AUDIO_FORMAT_S24_3LE;
      case SND_PCM_FORMAT_S32_LE:
         return AUDIO_FORMAT_S32_LE;
      case SND_PCM_FORMAT_FLOAT_LE:
         return AUDIO_FORMAT_FLOAT32;
      default:
         return AUDIO_FORMAT_S16_LE;
   }
}

/**
 * @brief Configure ALSA hardware parameters
 */
static int configure_hw_params(snd_pcm_t *pcm,
                               const audio_stream_params_t *params,
                               audio_hw_params_t *hw_out,
                               snd_pcm_stream_t stream_type) {
   snd_pcm_hw_params_t *hw_params = NULL;
   int rc = 0;
   int dir = 0;

   snd_pcm_hw_params_alloca(&hw_params);

   rc = snd_pcm_hw_params_any(pcm, hw_params);
   if (rc < 0) {
      OLOG_ERROR("ALSA: Cannot get hardware parameters: %s", snd_strerror(rc));
      return rc;
   }

   /* Access type: interleaved */
   rc = snd_pcm_hw_params_set_access(pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
   if (rc < 0) {
      OLOG_ERROR("ALSA: Cannot set access type: %s", snd_strerror(rc));
      return rc;
   }

   /* Sample format */
   snd_pcm_format_t format = format_to_alsa(params->format);
   rc = snd_pcm_hw_params_set_format(pcm, hw_params, format);
   if (rc < 0) {
      OLOG_ERROR("ALSA: Cannot set format: %s", snd_strerror(rc));
      return rc;
   }

   /* Channels */
   rc = snd_pcm_hw_params_set_channels(pcm, hw_params, params->channels);
   if (rc < 0) {
      OLOG_ERROR("ALSA: Cannot set channels to %u: %s", params->channels, snd_strerror(rc));
      return rc;
   }

   /* Sample rate */
   unsigned int rate = params->sample_rate;
   rc = snd_pcm_hw_params_set_rate_near(pcm, hw_params, &rate, &dir);
   if (rc < 0) {
      OLOG_ERROR("ALSA: Cannot set rate: %s", snd_strerror(rc));
      return rc;
   }
   if (rate != params->sample_rate) {
      OLOG_WARNING("ALSA: Rate %u not supported, using %u", params->sample_rate, rate);
   }

   /* Period size */
   snd_pcm_uframes_t period_frames = params->period_frames;
   rc = snd_pcm_hw_params_set_period_size_near(pcm, hw_params, &period_frames, &dir);
   if (rc < 0) {
      OLOG_ERROR("ALSA: Cannot set period size: %s", snd_strerror(rc));
      return rc;
   }

   /* Buffer size */
   snd_pcm_uframes_t buffer_frames = params->buffer_frames;
   if (buffer_frames > 0) {
      rc = snd_pcm_hw_params_set_buffer_size_near(pcm, hw_params, &buffer_frames);
      if (rc < 0) {
         OLOG_WARNING("ALSA: Cannot set buffer size, using default: %s", snd_strerror(rc));
      }
   }

   /* Apply parameters */
   rc = snd_pcm_hw_params(pcm, hw_params);
   if (rc < 0) {
      OLOG_ERROR("ALSA: Cannot set hardware parameters: %s", snd_strerror(rc));
      return rc;
   }

   /* Read back actual parameters */
   if (hw_out) {
      snd_pcm_hw_params_get_rate(hw_params, &hw_out->sample_rate, &dir);
      snd_pcm_hw_params_get_channels(hw_params, &hw_out->channels);
      snd_pcm_hw_params_get_period_size(hw_params, &period_frames, &dir);
      hw_out->period_frames = period_frames;
      snd_pcm_hw_params_get_buffer_size(hw_params, &buffer_frames);
      hw_out->buffer_frames = buffer_frames;

      snd_pcm_format_t actual_format;
      snd_pcm_hw_params_get_format(hw_params, &actual_format);
      hw_out->format = alsa_to_format(actual_format);

      OLOG_INFO("ALSA %s: rate=%u ch=%u period=%zu buffer=%zu",
                stream_type == SND_PCM_STREAM_CAPTURE ? "capture" : "playback", hw_out->sample_rate,
                hw_out->channels, hw_out->period_frames, hw_out->buffer_frames);
   }

   return 0;
}

/* =============================================================================
 * ALSA Capture Implementation
 * ============================================================================= */

/* A device that isn't there (unplugged, or not back yet) is its caller's to
 * report: it may be waiting for it. */
static bool alsa_device_missing(int rc) {
   return rc == -ENOENT || rc == -ENODEV || rc == -ENXIO;
}

/* "hw:1" names a card by its index, which can change when USB devices are
 * plugged in again; "hw:CARD=<name>" stays on the same card.  Said once. */
static void alsa_warn_index_name(const char *device) {
   static bool warned = false;
   const char *p = strncmp(device, "plughw:", 7) == 0 ? device + 7
                   : strncmp(device, "hw:", 3) == 0   ? device + 3
                                                      : NULL;
   if (!warned && p && *p >= '0' && *p <= '9') {
      OLOG_WARNING("ALSA: device '%s' names a card by its index, which can change when a USB "
                   "device is plugged in again; use its CARD= name (arecord -L / aplay -L)",
                   device);
      warned = true;
   }
}

audio_stream_capture_handle_t *alsa_capture_open(const char *device,
                                                 const audio_stream_params_t *params,
                                                 audio_hw_params_t *hw_params) {
   int rc = 0;

   if (!device) {
      device = "default";
   }

   audio_stream_capture_handle_t *handle = alloc_capture_handle();
   if (!handle) {
      OLOG_ERROR("ALSA: No free capture handles available");
      return NULL;
   }

   OLOG_DEBUG("ALSA: Opening capture device: %s", device);
   alsa_warn_index_name(device);

   rc = snd_pcm_open(&handle->pcm, device, SND_PCM_STREAM_CAPTURE, 0);
   if (rc < 0) {
      if (alsa_device_missing(rc)) {
         OLOG_DEBUG("ALSA: capture device '%s' not available: %s", device, snd_strerror(rc));
      } else {
         OLOG_ERROR("ALSA: Cannot open capture device '%s': %s", device, snd_strerror(rc));
      }
      free_capture_handle(handle);
      return NULL;
   }

   rc = configure_hw_params(handle->pcm, params, &handle->hw_params, SND_PCM_STREAM_CAPTURE);
   if (rc < 0) {
      snd_pcm_close(handle->pcm);
      free_capture_handle(handle);
      return NULL;
   }

   handle->bytes_per_frame = audio_bytes_per_frame(handle->hw_params.format,
                                                   handle->hw_params.channels);

   if (hw_params) {
      *hw_params = handle->hw_params;
   }

   OLOG_INFO("ALSA capture: %s", device);
   return handle;
}

ssize_t alsa_capture_read(audio_stream_capture_handle_t *handle, void *buffer, size_t frames) {
   if (!validate_capture_handle(handle) || !buffer) {
      return -AUDIO_ERR_INVALID;
   }

   snd_pcm_sframes_t rc = snd_pcm_readi(handle->pcm, buffer, frames);

   if (rc < 0) {
      /* Map ALSA error to audio error (negative return) */
      int err = alsa_error_to_audio_error((int)rc);
      /* For overrun on capture, it's actually an overrun */
      return -err;
   }

   return rc;
}

ssize_t alsa_capture_avail(audio_stream_capture_handle_t *handle) {
   if (!validate_capture_handle(handle)) {
      return -AUDIO_ERR_INVALID;
   }

   snd_pcm_sframes_t avail = snd_pcm_avail(handle->pcm);
   if (avail < 0) {
      return -alsa_error_to_audio_error((int)avail);
   }
   return avail;
}

int alsa_capture_recover(audio_stream_capture_handle_t *handle, int err) {
   if (!validate_capture_handle(handle)) {
      return AUDIO_ERR_INVALID;
   }

   if (err == AUDIO_ERR_OVERRUN) {
      /* Overrun */
      OLOG_WARNING("ALSA capture: overrun, recovering");
      int rc = snd_pcm_prepare(handle->pcm);
      if (rc < 0) {
         OLOG_ERROR("ALSA capture: prepare failed: %s", snd_strerror(rc));
         return alsa_error_to_audio_error(rc);
      }
      return AUDIO_SUCCESS;
   } else if (err == AUDIO_ERR_SUSPENDED) {
      /* Suspended */
      OLOG_WARNING("ALSA capture: suspended, resuming");
      int rc;
      while ((rc = snd_pcm_resume(handle->pcm)) == -EAGAIN) {
         usleep(100000); /* 100ms */
      }
      if (rc < 0) {
         rc = snd_pcm_prepare(handle->pcm);
         if (rc < 0) {
            OLOG_ERROR("ALSA capture: prepare after suspend failed: %s", snd_strerror(rc));
            return alsa_error_to_audio_error(rc);
         }
      }
      return AUDIO_SUCCESS;
   }

   return AUDIO_ERR_UNKNOWN;
}

void alsa_capture_close(audio_stream_capture_handle_t *handle) {
   if (!validate_capture_handle(handle)) {
      return;
   }

   if (handle->pcm) {
      snd_pcm_drop(handle->pcm);
      snd_pcm_close(handle->pcm);
   }

   free_capture_handle(handle);
   OLOG_INFO("ALSA capture closed");
}

/* =============================================================================
 * ALSA Playback Implementation
 * ============================================================================= */

audio_stream_playback_handle_t *alsa_playback_open(const char *device,
                                                   const audio_stream_params_t *params,
                                                   audio_hw_params_t *hw_params) {
   int rc = 0;

   if (!device) {
      device = "default";
   }

   audio_stream_playback_handle_t *handle = alloc_playback_handle();
   if (!handle) {
      OLOG_ERROR("ALSA: No free playback handles available");
      return NULL;
   }

   OLOG_DEBUG("ALSA: Opening playback device: %s", device);
   alsa_warn_index_name(device);

   rc = snd_pcm_open(&handle->pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
   if (rc < 0) {
      if (alsa_device_missing(rc)) {
         OLOG_DEBUG("ALSA: playback device '%s' not available: %s", device, snd_strerror(rc));
      } else {
         OLOG_ERROR("ALSA: Cannot open playback device '%s': %s", device, snd_strerror(rc));
      }
      free_playback_handle(handle);
      return NULL;
   }

   rc = configure_hw_params(handle->pcm, params, &handle->hw_params, SND_PCM_STREAM_PLAYBACK);
   if (rc < 0) {
      snd_pcm_close(handle->pcm);
      free_playback_handle(handle);
      return NULL;
   }

   handle->bytes_per_frame = audio_bytes_per_frame(handle->hw_params.format,
                                                   handle->hw_params.channels);

   if (hw_params) {
      *hw_params = handle->hw_params;
   }

   OLOG_INFO("ALSA playback: %s", device);
   return handle;
}

ssize_t alsa_playback_write(audio_stream_playback_handle_t *handle,
                            const void *buffer,
                            size_t frames) {
   if (!validate_playback_handle(handle) || !buffer) {
      return -AUDIO_ERR_INVALID;
   }

   snd_pcm_sframes_t rc = snd_pcm_writei(handle->pcm, buffer, frames);

   if (rc < 0) {
      /* Map ALSA error to audio error */
      int err = alsa_error_to_audio_error((int)rc);
      /* For underrun on playback, use UNDERRUN */
      if (rc == -EPIPE) {
         return -AUDIO_ERR_UNDERRUN;
      }
      return -err;
   }

   return rc;
}

ssize_t alsa_playback_avail(audio_stream_playback_handle_t *handle) {
   if (!validate_playback_handle(handle)) {
      return -AUDIO_ERR_INVALID;
   }

   snd_pcm_sframes_t avail = snd_pcm_avail(handle->pcm);
   if (avail < 0) {
      return -alsa_error_to_audio_error((int)avail);
   }
   return avail;
}

int alsa_playback_drain(audio_stream_playback_handle_t *handle) {
   if (!validate_playback_handle(handle)) {
      return AUDIO_ERR_INVALID;
   }

   int rc = snd_pcm_drain(handle->pcm);
   if (rc < 0) {
      OLOG_ERROR("ALSA playback: drain failed: %s", snd_strerror(rc));
      return alsa_error_to_audio_error(rc);
   }

   /* Re-prepare the PCM device after drain so it's ready for subsequent writes.
    * After snd_pcm_drain() completes, the PCM is in SND_PCM_STATE_SETUP state
    * and cannot accept writes until snd_pcm_prepare() is called. */
   rc = snd_pcm_prepare(handle->pcm);
   if (rc < 0) {
      OLOG_ERROR("ALSA playback: prepare after drain failed: %s", snd_strerror(rc));
      return alsa_error_to_audio_error(rc);
   }

   return AUDIO_SUCCESS;
}

int alsa_playback_drop(audio_stream_playback_handle_t *handle) {
   if (!validate_playback_handle(handle)) {
      return AUDIO_ERR_INVALID;
   }

   int rc = snd_pcm_drop(handle->pcm);
   if (rc < 0) {
      OLOG_ERROR("ALSA playback: drop failed: %s", snd_strerror(rc));
      return alsa_error_to_audio_error(rc);
   }

   /* Don't drain on close after explicit drop */
   handle->drain_on_close = false;

   return AUDIO_SUCCESS;
}

int alsa_playback_recover(audio_stream_playback_handle_t *handle, int err) {
   if (!validate_playback_handle(handle)) {
      return AUDIO_ERR_INVALID;
   }

   if (err == AUDIO_ERR_UNDERRUN) {
      /* Underrun */
      OLOG_WARNING("ALSA playback: underrun, recovering");
      int rc = snd_pcm_prepare(handle->pcm);
      if (rc < 0) {
         OLOG_ERROR("ALSA playback: prepare failed: %s", snd_strerror(rc));
         return alsa_error_to_audio_error(rc);
      }
      return AUDIO_SUCCESS;
   } else if (err == AUDIO_ERR_SUSPENDED) {
      /* Suspended */
      OLOG_WARNING("ALSA playback: suspended, resuming");
      int rc;
      while ((rc = snd_pcm_resume(handle->pcm)) == -EAGAIN) {
         usleep(100000); /* 100ms */
      }
      if (rc < 0) {
         rc = snd_pcm_prepare(handle->pcm);
         if (rc < 0) {
            OLOG_ERROR("ALSA playback: prepare after suspend failed: %s", snd_strerror(rc));
            return alsa_error_to_audio_error(rc);
         }
      }
      return AUDIO_SUCCESS;
   }

   return AUDIO_ERR_UNKNOWN;
}

void alsa_playback_close(audio_stream_playback_handle_t *handle) {
   if (!validate_playback_handle(handle)) {
      return;
   }

   if (handle->pcm) {
      /* Only drain if not previously dropped */
      if (handle->drain_on_close) {
         int rc = snd_pcm_drain(handle->pcm);
         if (rc < 0) {
            OLOG_WARNING("ALSA playback: drain on close failed: %s", snd_strerror(rc));
         }
      }
      snd_pcm_close(handle->pcm);
   }

   free_playback_handle(handle);
   OLOG_INFO("ALSA playback closed");
}
