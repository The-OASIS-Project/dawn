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
 * Ogg Vorbis Decoder Implementation
 *
 * Implements the audio_decoder vtable interface using libvorbis (vorbisfile).
 * Conditionally compiled when DAWN_ENABLE_OGG is defined.
 */

#ifdef DAWN_ENABLE_OGG

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <vorbis/vorbisfile.h>

#include "audio/audio_decoder.h"
#include "audio_decoder_internal.h"
#include "audio_downmix.h"
#include "core/path_utils.h"
#include "dawn_error.h"
#include "logging.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

/* Channels handed to the caller: a file with more is downmixed to stereo */
#define OGG_OUTPUT_CHANNELS 2

/* =============================================================================
 * Ogg Vorbis Decoder Handle
 * ============================================================================= */

/**
 * @brief Ogg Vorbis-specific decoder handle
 *
 * Embeds base audio_decoder_base_t as first member for safe casting.
 */
typedef struct {
   /* Base must be first member */
   audio_decoder_base_t base;

   /* libvorbis file handle */
   OggVorbis_File vf;
   bool vf_open;

   /* Stream info */
   long sample_rate;
   int channels;     /* Channels in the file */
   int out_channels; /* Channels returned by read(): the file's, at most 2 */
   ogg_int64_t total_samples;

   /* Weights for a file with more than OGG_OUTPUT_CHANNELS channels */
   audio_downmix_t downmix;

   /* Current logical bitstream index (for chained streams) */
   int current_section;

   /* State flags */
   bool eof;
   bool error;
} ogg_decoder_handle_t;

/* Forward declarations for vtable */
extern const struct audio_decoder_vtable *ogg_get_vtable(void);

/* =============================================================================
 * Vtable Implementation
 * ============================================================================= */

audio_decoder_t *ogg_decoder_open(const char *path) {
   ogg_decoder_handle_t *handle = calloc(1, sizeof(ogg_decoder_handle_t));
   if (!handle) {
      OLOG_ERROR("Failed to allocate Ogg decoder handle");
      return NULL;
   }

   /* Initialize base */
   handle->base.vtable = ogg_get_vtable();
   handle->base.format = AUDIO_FORMAT_OGG_VORBIS;

   /* Open the Ogg file */
   int err = ov_fopen(path, &handle->vf);
   if (err != 0) {
      const char *err_str;
      switch (err) {
         case OV_EREAD:
            err_str = "Read error";
            break;
         case OV_ENOTVORBIS:
            err_str = "Not a Vorbis file";
            break;
         case OV_EVERSION:
            err_str = "Version mismatch";
            break;
         case OV_EBADHEADER:
            err_str = "Bad header";
            break;
         case OV_EFAULT:
            err_str = "Internal error";
            break;
         default:
            err_str = "Unknown error";
            break;
      }
      OLOG_ERROR("Failed to open Ogg file '%s': %s (%d)", path, err_str, err);
      free(handle);
      return NULL;
   }
   handle->vf_open = true;

   /* Get stream info */
   vorbis_info *vi = ov_info(&handle->vf, -1);
   if (!vi) {
      OLOG_ERROR("Failed to get Ogg stream info");
      ov_clear(&handle->vf);
      free(handle);
      return NULL;
   }

   handle->sample_rate = vi->rate;
   handle->channels = vi->channels;
   handle->out_channels = vi->channels > OGG_OUTPUT_CHANNELS ? OGG_OUTPUT_CHANNELS : vi->channels;

   if (handle->channels > OGG_OUTPUT_CHANNELS) {
      if (audio_downmix_init(&handle->downmix, handle->channels, AUDIO_CHANNEL_ORDER_VORBIS) !=
          SUCCESS) {
         OLOG_ERROR("Ogg: can't downmix %d channels: %s", handle->channels, path);
         ov_clear(&handle->vf);
         free(handle);
         return NULL;
      }
      OLOG_INFO("Ogg: downmixing %d channels to stereo", handle->channels);
   }

   /* Get total samples (returns -1 for unseekable streams) */
   handle->total_samples = ov_pcm_total(&handle->vf, -1);

   OLOG_INFO("Ogg: %ldHz %dch, %s samples", handle->sample_rate, handle->channels,
             handle->total_samples >= 0 ? "known" : "unknown");

   return (audio_decoder_t *)handle;
}

void ogg_decoder_close(audio_decoder_t *dec) {
   if (!dec) {
      return;
   }

   ogg_decoder_handle_t *handle = (ogg_decoder_handle_t *)dec;

   if (handle->vf_open) {
      ov_clear(&handle->vf);
      handle->vf_open = false;
   }

   free(handle);
}

int ogg_decoder_get_info(audio_decoder_t *dec, audio_decoder_info_t *info) {
   if (!dec || !info) {
      return AUDIO_DECODER_ERR_INVALID;
   }

   ogg_decoder_handle_t *handle = (ogg_decoder_handle_t *)dec;

   info->sample_rate = (uint32_t)handle->sample_rate;
   info->channels = (uint8_t)handle->out_channels;
   info->bits_per_sample = 16; /* We request 16-bit output */
   info->total_samples = handle->total_samples >= 0 ? (uint64_t)handle->total_samples : 0;
   info->format = AUDIO_FORMAT_OGG_VORBIS;

   return AUDIO_DECODER_SUCCESS;
}

/*
 * A chained stream can change its channel count or rate between sections, but
 * the caller's buffer, the downmix and the caller's resampler are set up from
 * the first.  A section that differs ends the track (its data is discarded)
 * rather than being misread or played at the wrong pitch.
 */
static bool ogg_section_matches(ogg_decoder_handle_t *handle) {
   vorbis_info *vi = ov_info(&handle->vf, -1);
   if (vi && vi->channels == handle->channels && vi->rate == handle->sample_rate) {
      return true;
   }
   OLOG_ERROR("Ogg: chained section is %d ch %ld Hz, stream is %d ch %ld Hz", vi ? vi->channels : 0,
              vi ? vi->rate : 0L, handle->channels, handle->sample_rate);
   handle->error = true;
   return false;
}

ssize_t ogg_decoder_read(audio_decoder_t *dec, int16_t *buffer, size_t max_frames) {
   if (!dec || !buffer || max_frames == 0) {
      return -AUDIO_DECODER_ERR_INVALID;
   }

   ogg_decoder_handle_t *handle = (ogg_decoder_handle_t *)dec;

   if (handle->error) {
      return -AUDIO_DECODER_ERR_READ;
   }

   if (handle->eof) {
      return 0;
   }

   size_t frame_bytes = (size_t)handle->out_channels * sizeof(int16_t);
   size_t bytes_wanted = max_frames * frame_bytes;
   size_t bytes_read = 0;
   char *buf_ptr = (char *)buffer;

   while (bytes_read < bytes_wanted) {
      long result;
      if (handle->channels > OGG_OUTPUT_CHANNELS) {
         /* Planar float, downmixed into the caller's stereo buffer */
         float **pcm = NULL;
         int frames_left = (int)((bytes_wanted - bytes_read) / frame_bytes);
         result = ov_read_float(&handle->vf, &pcm, frames_left, &handle->current_section);
         if (result > 0) {
            if (!ogg_section_matches(handle)) {
               break;
            }
            audio_downmix_float(&handle->downmix, (const float *const *)pcm, (size_t)result,
                                (int16_t *)(buf_ptr + bytes_read));
            result *= (long)frame_bytes;
         }
      } else {
         /* Read up to bytes_wanted bytes
          * Parameters: vf, buffer, length, bigendianp, word, sgned, current_section
          * - bigendianp: 0 for little-endian
          * - word: 2 for 16-bit samples
          * - sgned: 1 for signed samples
          */
         result = ov_read(&handle->vf, buf_ptr + bytes_read, (int)(bytes_wanted - bytes_read),
                          0, /* little-endian */
                          2, /* 16-bit */
                          1, /* signed */
                          &handle->current_section);
         if (result > 0 && !ogg_section_matches(handle)) {
            break;
         }
      }

      if (result == 0) {
         /* EOF */
         handle->eof = true;
         break;
      } else if (result < 0) {
         /* Error codes */
         switch (result) {
            case OV_HOLE:
               /* Interruption in data - skip and continue */
               OLOG_WARNING("Ogg: Data hole in stream, continuing");
               continue;
            case OV_EBADLINK:
               OLOG_ERROR("Ogg: Bad link in stream");
               handle->error = true;
               break;
            case OV_EINVAL:
               OLOG_ERROR("Ogg: Invalid argument");
               handle->error = true;
               break;
            default:
               OLOG_ERROR("Ogg: Unknown read error %ld", result);
               handle->error = true;
               break;
         }
         if (handle->error) {
            if (bytes_read == 0) {
               return -AUDIO_DECODER_ERR_READ;
            }
            break;
         }
      } else {
         bytes_read += (size_t)result;
      }
   }

   if (handle->error && bytes_read == 0) {
      return -AUDIO_DECODER_ERR_READ;
   }

   /* Convert bytes to frames */
   size_t frames_read = bytes_read / frame_bytes;
   return (ssize_t)frames_read;
}

int ogg_decoder_seek(audio_decoder_t *dec, uint64_t sample_pos) {
   if (!dec) {
      return AUDIO_DECODER_ERR_INVALID;
   }

   ogg_decoder_handle_t *handle = (ogg_decoder_handle_t *)dec;

   int result = ov_pcm_seek(&handle->vf, (ogg_int64_t)sample_pos);
   if (result != 0) {
      const char *err_str;
      switch (result) {
         case OV_ENOSEEK:
            err_str = "Stream is not seekable";
            break;
         case OV_EINVAL:
            err_str = "Invalid argument";
            break;
         case OV_EREAD:
            err_str = "Read error";
            break;
         case OV_EFAULT:
            err_str = "Internal error";
            break;
         case OV_EBADLINK:
            err_str = "Bad link";
            break;
         default:
            err_str = "Unknown error";
            break;
      }
      OLOG_WARNING("Ogg seek failed: %s (%d)", err_str, result);
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
 * @brief Extract metadata (title, artist, album) from Ogg Vorbis comments
 *
 * Uses libvorbis comment query functions to extract tags.
 *
 * @param path Path to Ogg file
 * @param metadata Output structure
 * @return AUDIO_DECODER_SUCCESS on success, or error code
 */
int ogg_get_metadata(const char *path, audio_metadata_t *metadata) {
   if (!path || !metadata) {
      return AUDIO_DECODER_ERR_INVALID;
   }

   memset(metadata, 0, sizeof(*metadata));

   OggVorbis_File vf;
   int err = ov_fopen(path, &vf);
   if (err != 0) {
      return AUDIO_DECODER_ERR_OPEN;
   }

   /* Get duration */
   double duration = ov_time_total(&vf, -1);
   if (duration > 0) {
      metadata->duration_sec = (uint32_t)duration;
   }

   /* Get Vorbis comments */
   vorbis_comment *vc = ov_comment(&vf, -1);
   if (vc) {
      char *val;

      /* vorbis_comment_query returns the first matching value */
      if ((val = vorbis_comment_query(vc, "TITLE", 0)) != NULL) {
         safe_strncpy(metadata->title, val, AUDIO_METADATA_STRING_MAX);
      }
      if ((val = vorbis_comment_query(vc, "ARTIST", 0)) != NULL) {
         safe_strncpy(metadata->artist, val, AUDIO_METADATA_STRING_MAX);
      }
      if ((val = vorbis_comment_query(vc, "ALBUM", 0)) != NULL) {
         safe_strncpy(metadata->album, val, AUDIO_METADATA_STRING_MAX);
      }
      /* GENRE may repeat; comma-join every entry for parity with the Plex path. */
      for (int gi = 0; (val = vorbis_comment_query(vc, "GENRE", gi)) != NULL; gi++) {
         audio_metadata_append_genre(metadata, val);
      }
      /* Prefer DATE (may be a full date), fall back to YEAR. */
      if ((val = vorbis_comment_query(vc, "DATE", 0)) != NULL) {
         metadata->year = audio_metadata_parse_year(val);
      }
      if (metadata->year == 0 && (val = vorbis_comment_query(vc, "YEAR", 0)) != NULL) {
         metadata->year = audio_metadata_parse_year(val);
      }

      metadata->has_metadata = (metadata->title[0] || metadata->artist[0] || metadata->album[0] ||
                                metadata->genre[0]);
   }

   ov_clear(&vf);
   return AUDIO_DECODER_SUCCESS;
}

/* =============================================================================
 * Vtable Export
 * ============================================================================= */

/* Vtable defined in audio_decoder_internal.h */

static const char *ogg_ext_list[] = { ".ogg", ".oga", NULL };

static const audio_decoder_vtable_t g_ogg_decoder_vtable = {
   .name = "Ogg Vorbis",
   .extensions = ogg_ext_list,
   .format = AUDIO_FORMAT_OGG_VORBIS,
   .open = ogg_decoder_open,
   .close = ogg_decoder_close,
   .get_info = ogg_decoder_get_info,
   .read = ogg_decoder_read,
   .seek = ogg_decoder_seek,
};

const audio_decoder_vtable_t *ogg_get_vtable(void) {
   return &g_ogg_decoder_vtable;
}

#endif /* DAWN_ENABLE_OGG */
