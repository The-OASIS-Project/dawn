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
 * MP3 Decoder Implementation
 *
 * Implements the audio_decoder vtable interface using libmpg123.
 * Conditionally compiled when DAWN_ENABLE_MP3 is defined.
 */

#ifdef DAWN_ENABLE_MP3

#include <mpg123.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_decoder.h"
#include "audio_decoder_internal.h"
#include "core/path_utils.h"
#include "logging.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

/* Maximum channels we support (stereo) */
#define MP3_MAX_CHANNELS 2

/* =============================================================================
 * MP3 Decoder Handle
 * ============================================================================= */

/**
 * @brief MP3-specific decoder handle
 *
 * Embeds base audio_decoder_base_t as first member for safe casting.
 */
typedef struct {
   /* Base must be first member */
   audio_decoder_base_t base;

   /* libmpg123 decoder instance */
   mpg123_handle *mpg;

   /* Stream info */
   long sample_rate;
   int channels;
   int encoding;
   off_t total_samples; /* May be MPG123_ERR (-1) if unknown */

   /* State flags */
   bool eof;
   bool error;
} mp3_decoder_handle_t;

/* Forward declarations for vtable */
extern const struct audio_decoder_vtable *mp3_get_vtable(void);

/* =============================================================================
 * Vtable Implementation
 * ============================================================================= */

audio_decoder_t *mp3_decoder_open(const char *path) {
   int err;

   mp3_decoder_handle_t *handle = calloc(1, sizeof(mp3_decoder_handle_t));
   if (!handle) {
      OLOG_ERROR("Failed to allocate MP3 decoder handle");
      return NULL;
   }

   /* Initialize base */
   handle->base.vtable = mp3_get_vtable();
   handle->base.format = AUDIO_FORMAT_MP3;

   /* Create mpg123 handle */
   handle->mpg = mpg123_new(NULL, &err);
   if (!handle->mpg) {
      OLOG_ERROR("Failed to create mpg123 handle: %s", mpg123_plain_strerror(err));
      free(handle);
      return NULL;
   }

   /* Force output format to signed 16-bit for consistency */
   mpg123_format_none(handle->mpg);
   mpg123_format(handle->mpg, 44100, MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
   mpg123_format(handle->mpg, 48000, MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
   mpg123_format(handle->mpg, 22050, MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
   mpg123_format(handle->mpg, 32000, MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
   mpg123_format(handle->mpg, 16000, MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);

   /* Open the file */
   err = mpg123_open(handle->mpg, path);
   if (err != MPG123_OK) {
      OLOG_ERROR("Failed to open MP3 file '%s': %s", path, mpg123_strerror(handle->mpg));
      mpg123_delete(handle->mpg);
      free(handle);
      return NULL;
   }

   /* Get format info */
   err = mpg123_getformat(handle->mpg, &handle->sample_rate, &handle->channels, &handle->encoding);
   if (err != MPG123_OK) {
      OLOG_ERROR("Failed to get MP3 format: %s", mpg123_strerror(handle->mpg));
      mpg123_close(handle->mpg);
      mpg123_delete(handle->mpg);
      free(handle);
      return NULL;
   }

   /* Get total length if available (VBR files may not have accurate length) */
   handle->total_samples = mpg123_length(handle->mpg);

   OLOG_INFO("MP3: %ldHz %dch, %s samples", handle->sample_rate, handle->channels,
             handle->total_samples >= 0 ? "known" : "unknown");

   return (audio_decoder_t *)handle;
}

void mp3_decoder_close(audio_decoder_t *dec) {
   if (!dec) {
      return;
   }

   mp3_decoder_handle_t *handle = (mp3_decoder_handle_t *)dec;

   if (handle->mpg) {
      mpg123_close(handle->mpg);
      mpg123_delete(handle->mpg);
      handle->mpg = NULL;
   }

   free(handle);
}

int mp3_decoder_get_info(audio_decoder_t *dec, audio_decoder_info_t *info) {
   if (!dec || !info) {
      return AUDIO_DECODER_ERR_INVALID;
   }

   mp3_decoder_handle_t *handle = (mp3_decoder_handle_t *)dec;

   info->sample_rate = (uint32_t)handle->sample_rate;
   info->channels = (uint8_t)handle->channels;
   info->bits_per_sample = 16; /* We force 16-bit output */
   info->total_samples = handle->total_samples >= 0 ? (uint64_t)handle->total_samples : 0;
   info->format = AUDIO_FORMAT_MP3;

   return AUDIO_DECODER_SUCCESS;
}

ssize_t mp3_decoder_read(audio_decoder_t *dec, int16_t *buffer, size_t max_frames) {
   if (!dec || !buffer || max_frames == 0) {
      return -AUDIO_DECODER_ERR_INVALID;
   }

   mp3_decoder_handle_t *handle = (mp3_decoder_handle_t *)dec;

   if (handle->error) {
      return -AUDIO_DECODER_ERR_READ;
   }

   if (handle->eof) {
      return 0;
   }

   size_t bytes_wanted = max_frames * handle->channels * sizeof(int16_t);
   size_t bytes_read = 0;

   int err = mpg123_read(handle->mpg, (unsigned char *)buffer, bytes_wanted, &bytes_read);

   if (err == MPG123_DONE) {
      handle->eof = true;
      /* Return whatever we got */
   } else if (err != MPG123_OK && err != MPG123_NEW_FORMAT) {
      OLOG_ERROR("MP3 decode error: %s", mpg123_strerror(handle->mpg));
      handle->error = true;
      if (bytes_read == 0) {
         return -AUDIO_DECODER_ERR_READ;
      }
      /* Return whatever we got before the error */
   }

   /* Convert bytes to frames */
   size_t frames_read = bytes_read / (handle->channels * sizeof(int16_t));
   return (ssize_t)frames_read;
}

int mp3_decoder_seek(audio_decoder_t *dec, uint64_t sample_pos) {
   if (!dec) {
      return AUDIO_DECODER_ERR_INVALID;
   }

   mp3_decoder_handle_t *handle = (mp3_decoder_handle_t *)dec;

   off_t result = mpg123_seek(handle->mpg, (off_t)sample_pos, SEEK_SET);
   if (result < 0) {
      OLOG_WARNING("MP3 seek failed: %s", mpg123_strerror(handle->mpg));
      return AUDIO_DECODER_ERR_SEEK;
   }

   handle->eof = false;
   return AUDIO_DECODER_SUCCESS;
}

/* =============================================================================
 * Metadata Extraction
 * ============================================================================= */

/**
 * @brief Safe string copy with null termination
 */
/**
 * @brief Copy mpg123 string to output buffer
 *
 * mpg123 provides strings as mpg123_string with separate data and size.
 */
static void copy_mpg123_string(char *dst, size_t dst_size, mpg123_string *src) {
   if (!dst || dst_size == 0)
      return;
   if (!src || !src->p || src->fill == 0) {
      dst[0] = '\0';
      return;
   }
   /* src->fill includes null terminator */
   size_t copy_len = src->fill - 1;
   if (copy_len >= dst_size) {
      copy_len = dst_size - 1;
   }
   memcpy(dst, src->p, copy_len);
   dst[copy_len] = '\0';
}

/**
 * @brief Trim trailing spaces from ID3v1 fixed-width fields
 */
static void trim_trailing_spaces(char *str) {
   if (!str)
      return;
   size_t len = strlen(str);
   while (len > 0 && str[len - 1] == ' ') {
      str[--len] = '\0';
   }
}

/**
 * @brief Extract metadata (title, artist, album) from MP3 ID3 tags
 *
 * Uses mpg123's ID3 parsing to extract both ID3v1 and ID3v2 tags.
 * ID3v2 takes priority if present.
 *
 * @param path Path to MP3 file
 * @param metadata Output structure
 * @return AUDIO_DECODER_SUCCESS on success, or error code
 */
int mp3_get_metadata(const char *path, audio_metadata_t *metadata) {
   if (!path || !metadata) {
      return AUDIO_DECODER_ERR_INVALID;
   }

   memset(metadata, 0, sizeof(*metadata));

   int err;
   mpg123_handle *mpg = mpg123_new(NULL, &err);
   if (!mpg) {
      return AUDIO_DECODER_ERR_MEMORY;
   }

   /* We need to scan the file for ID3 tags */
   if (mpg123_open(mpg, path) != MPG123_OK) {
      mpg123_delete(mpg);
      return AUDIO_DECODER_ERR_OPEN;
   }

   /* Scan for metadata - this reads ID3 tags */
   mpg123_scan(mpg);

   /* Get duration */
   off_t length = mpg123_length(mpg);
   if (length >= 0) {
      long rate;
      int channels, encoding;
      if (mpg123_getformat(mpg, &rate, &channels, &encoding) == MPG123_OK && rate > 0) {
         metadata->duration_sec = (uint32_t)(length / rate);
      }
   }

   /* Check what metadata is available */
   int meta = mpg123_meta_check(mpg);

   /* Try ID3v2 first (higher priority, more complete) */
   if (meta & MPG123_ID3) {
      mpg123_id3v1 *v1 = NULL;
      mpg123_id3v2 *v2 = NULL;

      if (mpg123_id3(mpg, &v1, &v2) == MPG123_OK) {
         /* Prefer ID3v2 */
         if (v2) {
            if (v2->title)
               copy_mpg123_string(metadata->title, AUDIO_METADATA_STRING_MAX, v2->title);
            if (v2->artist)
               copy_mpg123_string(metadata->artist, AUDIO_METADATA_STRING_MAX, v2->artist);
            if (v2->album)
               copy_mpg123_string(metadata->album, AUDIO_METADATA_STRING_MAX, v2->album);
         }

         /* Fall back to ID3v1 for any missing fields */
         if (v1) {
            if (!metadata->title[0] && v1->title[0]) {
               safe_strncpy(metadata->title, v1->title, 31);
               trim_trailing_spaces(metadata->title);
            }
            if (!metadata->artist[0] && v1->artist[0]) {
               safe_strncpy(metadata->artist, v1->artist, 31);
               trim_trailing_spaces(metadata->artist);
            }
            if (!metadata->album[0] && v1->album[0]) {
               safe_strncpy(metadata->album, v1->album, 31);
               trim_trailing_spaces(metadata->album);
            }
         }
      }
   }

   metadata->has_metadata = (metadata->title[0] || metadata->artist[0] || metadata->album[0]);

   mpg123_close(mpg);
   mpg123_delete(mpg);

   return AUDIO_DECODER_SUCCESS;
}

/* =============================================================================
 * Vtable Export
 * ============================================================================= */

/* Vtable defined in audio_decoder_internal.h */

static const char *mp3_ext_list[] = { ".mp3", NULL };

static const audio_decoder_vtable_t g_mp3_decoder_vtable = {
   .name = "MP3",
   .extensions = mp3_ext_list,
   .format = AUDIO_FORMAT_MP3,
   .open = mp3_decoder_open,
   .close = mp3_decoder_close,
   .get_info = mp3_decoder_get_info,
   .read = mp3_decoder_read,
   .seek = mp3_decoder_seek,
};

const audio_decoder_vtable_t *mp3_get_vtable(void) {
   return &g_mp3_decoder_vtable;
}

/* =============================================================================
 * Library Initialization (called from audio_decoder_init/cleanup)
 * ============================================================================= */

static bool g_mp3_initialized = false;

/**
 * @brief Initialize mpg123 library
 *
 * Called from audio_decoder_init(). Must be called before any MP3 decoding.
 *
 * @return AUDIO_DECODER_SUCCESS on success, error code on failure
 */
int mp3_decoder_lib_init(void) {
   if (g_mp3_initialized) {
      return AUDIO_DECODER_SUCCESS;
   }

   int err = mpg123_init();
   if (err != MPG123_OK) {
      OLOG_ERROR("Failed to initialize mpg123 library: %s", mpg123_plain_strerror(err));
      return AUDIO_DECODER_ERR_NOT_INIT;
   }

   g_mp3_initialized = true;
   OLOG_INFO("mpg123 library initialized");
   return AUDIO_DECODER_SUCCESS;
}

/**
 * @brief Clean up mpg123 library
 *
 * Called from audio_decoder_cleanup().
 */
void mp3_decoder_lib_cleanup(void) {
   if (g_mp3_initialized) {
      mpg123_exit();
      g_mp3_initialized = false;
      OLOG_INFO("mpg123 library cleaned up");
   }
}

#endif /* DAWN_ENABLE_MP3 */
