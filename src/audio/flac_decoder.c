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
 * FLAC Decoder Implementation
 *
 * Implements the audio_decoder vtable interface using libFLAC.
 * Uses a streaming decoder with internal buffering to provide the read() API.
 */

#include <FLAC/metadata.h>
#include <FLAC/stream_decoder.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* For strncasecmp */

#include "audio/audio_decoder.h"
#include "audio_decoder_internal.h"
#include "core/path_utils.h"
#include "logging.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

/* Maximum block size for FLAC (per spec: 65535, but 8192 is typical) */
#define FLAC_MAX_BLOCK_SIZE 8192

/* Maximum channels we support (stereo) */
#define FLAC_MAX_CHANNELS 2

/* Internal buffer size in frames (must hold at least one FLAC block) */
#define FLAC_BUFFER_FRAMES FLAC_MAX_BLOCK_SIZE

/* =============================================================================
 * FLAC Decoder Handle
 * ============================================================================= */

/**
 * @brief FLAC-specific decoder handle
 *
 * Embeds base audio_decoder_base_t as first member for safe casting.
 */
typedef struct {
   /* Base must be first member */
   audio_decoder_base_t base;

   /* libFLAC decoder instance */
   FLAC__StreamDecoder *flac;

   /* Stream info (populated from metadata callback) */
   uint32_t sample_rate;
   uint8_t channels;
   uint8_t bits_per_sample;
   uint64_t total_samples;

   /* Internal sample buffer (interleaved 16-bit) */
   int16_t *buffer;
   size_t buffer_frames;   /* Buffer capacity in frames */
   size_t buffered_frames; /* Currently buffered frames */
   size_t read_position;   /* Read position within buffer (in frames) */

   /* State flags */
   bool eof;               /* End of file reached */
   bool error;             /* Decode error occurred */
   bool metadata_received; /* Metadata callback was invoked */
} flac_decoder_handle_t;

/* Forward declarations for vtable */
extern const struct audio_decoder_vtable *flac_get_vtable(void);

/* =============================================================================
 * libFLAC Callbacks
 * ============================================================================= */

/**
 * @brief Metadata callback - extracts stream info
 */
static void flac_metadata_callback(const FLAC__StreamDecoder *decoder,
                                   const FLAC__StreamMetadata *metadata,
                                   void *client_data) {
   flac_decoder_handle_t *handle = (flac_decoder_handle_t *)client_data;

   if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
      const FLAC__StreamMetadata_StreamInfo *info = &metadata->data.stream_info;

      handle->sample_rate = info->sample_rate;
      handle->channels = info->channels;
      handle->bits_per_sample = info->bits_per_sample;
      handle->total_samples = info->total_samples;
      handle->metadata_received = true;

      OLOG_INFO("FLAC: %uHz %uch %ubps, %llu samples", info->sample_rate, info->channels,
                info->bits_per_sample, (unsigned long long)info->total_samples);
   }
}

/**
 * @brief Error callback - logs decode errors
 */
static void flac_error_callback(const FLAC__StreamDecoder *decoder,
                                FLAC__StreamDecoderErrorStatus status,
                                void *client_data) {
   flac_decoder_handle_t *handle = (flac_decoder_handle_t *)client_data;
   handle->error = true;

   OLOG_ERROR("FLAC decode error: %s", FLAC__StreamDecoderErrorStatusString[status]);
}

/**
 * @brief Write callback - buffers decoded samples
 *
 * Converts from FLAC's 32-bit planar format to interleaved 16-bit.
 * Stores samples in handle's internal buffer for later retrieval via read().
 */
static FLAC__StreamDecoderWriteStatus flac_write_callback(const FLAC__StreamDecoder *decoder,
                                                          const FLAC__Frame *frame,
                                                          const FLAC__int32 *const buffer[],
                                                          void *client_data) {
   flac_decoder_handle_t *handle = (flac_decoder_handle_t *)client_data;

   /* Validate block size fits in buffer */
   if (frame->header.blocksize > handle->buffer_frames) {
      OLOG_ERROR("FLAC block size %u exceeds buffer %zu", frame->header.blocksize,
                 handle->buffer_frames);
      handle->error = true;
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
   }

   /* Validate channel count */
   if (frame->header.channels > FLAC_MAX_CHANNELS) {
      OLOG_ERROR("FLAC has %u channels, max supported is %d", frame->header.channels,
                 FLAC_MAX_CHANNELS);
      handle->error = true;
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
   }

   /* Convert to interleaved 16-bit samples */
   size_t out_idx = 0;
   for (unsigned int i = 0; i < frame->header.blocksize; i++) {
      for (unsigned int ch = 0; ch < frame->header.channels; ch++) {
         int32_t sample = buffer[ch][i];

         /* Scale to 16-bit based on bits_per_sample */
         if (handle->bits_per_sample > 16) {
            sample >>= (handle->bits_per_sample - 16);
         } else if (handle->bits_per_sample < 16) {
            sample <<= (16 - handle->bits_per_sample);
         }

         /* Clipping protection */
         if (sample < INT16_MIN) {
            sample = INT16_MIN;
         } else if (sample > INT16_MAX) {
            sample = INT16_MAX;
         }

         handle->buffer[out_idx++] = (int16_t)sample;
      }
   }

   handle->buffered_frames = frame->header.blocksize;
   handle->read_position = 0;

   return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

/* =============================================================================
 * Vtable Implementation
 * ============================================================================= */

audio_decoder_t *flac_decoder_open(const char *path) {
   flac_decoder_handle_t *handle = calloc(1, sizeof(flac_decoder_handle_t));
   if (!handle) {
      OLOG_ERROR("Failed to allocate FLAC decoder handle");
      return NULL;
   }

   /* Initialize base */
   handle->base.vtable = flac_get_vtable();
   handle->base.format = AUDIO_FORMAT_FLAC;

   /* Allocate sample buffer (stereo, 16-bit) */
   handle->buffer_frames = FLAC_BUFFER_FRAMES;
   handle->buffer = malloc(FLAC_BUFFER_FRAMES * FLAC_MAX_CHANNELS * sizeof(int16_t));
   if (!handle->buffer) {
      OLOG_ERROR("Failed to allocate FLAC sample buffer");
      free(handle);
      return NULL;
   }

   /* Create FLAC decoder */
   handle->flac = FLAC__stream_decoder_new();
   if (!handle->flac) {
      OLOG_ERROR("Failed to create FLAC decoder");
      free(handle->buffer);
      free(handle);
      return NULL;
   }

   /* Initialize with file */
   FLAC__StreamDecoderInitStatus init_status = FLAC__stream_decoder_init_file(
       handle->flac, path, flac_write_callback, flac_metadata_callback, flac_error_callback,
       handle);

   if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
      OLOG_ERROR("FLAC init failed: %s", FLAC__StreamDecoderInitStatusString[init_status]);
      FLAC__stream_decoder_delete(handle->flac);
      free(handle->buffer);
      free(handle);
      return NULL;
   }

   /* Process until we get metadata (reads stream info) */
   if (!FLAC__stream_decoder_process_until_end_of_metadata(handle->flac)) {
      OLOG_ERROR("Failed to read FLAC metadata");
      FLAC__stream_decoder_finish(handle->flac);
      FLAC__stream_decoder_delete(handle->flac);
      free(handle->buffer);
      free(handle);
      return NULL;
   }

   if (!handle->metadata_received) {
      OLOG_ERROR("No FLAC metadata received");
      FLAC__stream_decoder_finish(handle->flac);
      FLAC__stream_decoder_delete(handle->flac);
      free(handle->buffer);
      free(handle);
      return NULL;
   }

   return (audio_decoder_t *)handle;
}

void flac_decoder_close(audio_decoder_t *dec) {
   if (!dec) {
      return;
   }

   flac_decoder_handle_t *handle = (flac_decoder_handle_t *)dec;

   if (handle->flac) {
      FLAC__stream_decoder_finish(handle->flac);
      FLAC__stream_decoder_delete(handle->flac);
      handle->flac = NULL;
   }

   free(handle->buffer);
   handle->buffer = NULL;

   free(handle);
}

int flac_decoder_get_info(audio_decoder_t *dec, audio_decoder_info_t *info) {
   if (!dec || !info) {
      return AUDIO_DECODER_ERR_INVALID;
   }

   flac_decoder_handle_t *handle = (flac_decoder_handle_t *)dec;

   info->sample_rate = handle->sample_rate;
   info->channels = handle->channels;
   info->bits_per_sample = handle->bits_per_sample;
   info->total_samples = handle->total_samples;
   info->format = AUDIO_FORMAT_FLAC;

   return AUDIO_DECODER_SUCCESS;
}

ssize_t flac_decoder_read(audio_decoder_t *dec, int16_t *buffer, size_t max_frames) {
   if (!dec || !buffer || max_frames == 0) {
      return -AUDIO_DECODER_ERR_INVALID;
   }

   flac_decoder_handle_t *handle = (flac_decoder_handle_t *)dec;

   if (handle->error) {
      return -AUDIO_DECODER_ERR_READ;
   }

   if (handle->eof) {
      return 0; /* EOF */
   }

   size_t frames_read = 0;
   size_t channels = handle->channels;

   while (frames_read < max_frames) {
      /* Check if we have buffered samples to return */
      size_t available = handle->buffered_frames - handle->read_position;

      if (available > 0) {
         /* Copy from internal buffer to caller's buffer */
         size_t to_copy = max_frames - frames_read;
         if (to_copy > available) {
            to_copy = available;
         }

         size_t sample_offset = handle->read_position * channels;
         size_t output_offset = frames_read * channels;
         memcpy(&buffer[output_offset], &handle->buffer[sample_offset],
                to_copy * channels * sizeof(int16_t));

         handle->read_position += to_copy;
         frames_read += to_copy;
      } else {
         /* Buffer empty, decode more samples */
         FLAC__StreamDecoderState state = FLAC__stream_decoder_get_state(handle->flac);

         if (state == FLAC__STREAM_DECODER_END_OF_STREAM) {
            handle->eof = true;
            break;
         }

         if (state >= FLAC__STREAM_DECODER_OGG_ERROR) {
            handle->error = true;
            return frames_read > 0 ? (ssize_t)frames_read : -AUDIO_DECODER_ERR_READ;
         }

         /* Decode one frame (populates internal buffer via callback) */
         handle->buffered_frames = 0;
         handle->read_position = 0;

         if (!FLAC__stream_decoder_process_single(handle->flac)) {
            if (handle->error) {
               return frames_read > 0 ? (ssize_t)frames_read : -AUDIO_DECODER_ERR_READ;
            }
            /* Might be EOF - check on next iteration */
         }
      }
   }

   return (ssize_t)frames_read;
}

int flac_decoder_seek(audio_decoder_t *dec, uint64_t sample_pos) {
   if (!dec) {
      return AUDIO_DECODER_ERR_INVALID;
   }

   flac_decoder_handle_t *handle = (flac_decoder_handle_t *)dec;

   if (!FLAC__stream_decoder_seek_absolute(handle->flac, sample_pos)) {
      /* Seek failed - check if stream is seekable */
      FLAC__StreamDecoderState state = FLAC__stream_decoder_get_state(handle->flac);
      if (state == FLAC__STREAM_DECODER_SEEK_ERROR) {
         /* Try to recover by flushing and resetting */
         FLAC__stream_decoder_flush(handle->flac);
      }
      return AUDIO_DECODER_ERR_SEEK;
   }

   /* Clear buffer after seek */
   handle->buffered_frames = 0;
   handle->read_position = 0;
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
 * @brief Extract metadata (title, artist, album) from FLAC Vorbis comments
 *
 * Uses libFLAC's metadata API to read Vorbis comment blocks without
 * decoding the audio data.
 *
 * @param path Path to FLAC file
 * @param metadata Output structure
 * @return AUDIO_DECODER_SUCCESS on success, or error code
 */
int flac_get_metadata(const char *path, audio_metadata_t *metadata) {
   if (!path || !metadata) {
      return AUDIO_DECODER_ERR_INVALID;
   }

   memset(metadata, 0, sizeof(*metadata));

   FLAC__Metadata_SimpleIterator *iter = FLAC__metadata_simple_iterator_new();
   if (!iter) {
      return AUDIO_DECODER_ERR_MEMORY;
   }

   if (!FLAC__metadata_simple_iterator_init(iter, path, true, false)) {
      FLAC__metadata_simple_iterator_delete(iter);
      return AUDIO_DECODER_ERR_OPEN;
   }

   /* Iterate through metadata blocks looking for VORBIS_COMMENT and STREAMINFO */
   do {
      FLAC__MetadataType type = FLAC__metadata_simple_iterator_get_block_type(iter);

      if (type == FLAC__METADATA_TYPE_STREAMINFO) {
         /* Get duration from stream info */
         FLAC__StreamMetadata *block = FLAC__metadata_simple_iterator_get_block(iter);
         if (block) {
            const FLAC__StreamMetadata_StreamInfo *info = &block->data.stream_info;
            if (info->sample_rate > 0 && info->total_samples > 0) {
               metadata->duration_sec = (uint32_t)(info->total_samples / info->sample_rate);
            }
            FLAC__metadata_object_delete(block);
         }
      } else if (type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
         /* Parse Vorbis comments for tags */
         FLAC__StreamMetadata *block = FLAC__metadata_simple_iterator_get_block(iter);
         if (block) {
            FLAC__StreamMetadata_VorbisComment *vc = &block->data.vorbis_comment;

            for (uint32_t i = 0; i < vc->num_comments; i++) {
               const char *entry = (const char *)vc->comments[i].entry;
               uint32_t length = vc->comments[i].length;

               if (!entry || length == 0) {
                  continue;
               }

               /* Parse "KEY=VALUE" format (case-insensitive key) */
               if (strncasecmp(entry, "TITLE=", 6) == 0 && length > 6) {
                  safe_strncpy(metadata->title, entry + 6, AUDIO_METADATA_STRING_MAX);
               } else if (strncasecmp(entry, "ARTIST=", 7) == 0 && length > 7) {
                  safe_strncpy(metadata->artist, entry + 7, AUDIO_METADATA_STRING_MAX);
               } else if (strncasecmp(entry, "ALBUM=", 6) == 0 && length > 6) {
                  safe_strncpy(metadata->album, entry + 6, AUDIO_METADATA_STRING_MAX);
               }
            }

            metadata->has_metadata = (metadata->title[0] || metadata->artist[0] ||
                                      metadata->album[0]);
            FLAC__metadata_object_delete(block);
         }
      }
   } while (FLAC__metadata_simple_iterator_next(iter));

   FLAC__metadata_simple_iterator_delete(iter);
   return AUDIO_DECODER_SUCCESS;
}

/* =============================================================================
 * Vtable Export
 * ============================================================================= */

/* Vtable defined in audio_decoder_internal.h */

static const char *flac_ext_list[] = { ".flac", NULL };

static const audio_decoder_vtable_t g_flac_decoder_vtable = {
   .name = "FLAC",
   .extensions = flac_ext_list,
   .format = AUDIO_FORMAT_FLAC,
   .open = flac_decoder_open,
   .close = flac_decoder_close,
   .get_info = flac_decoder_get_info,
   .read = flac_decoder_read,
   .seek = flac_decoder_seek,
};

const audio_decoder_vtable_t *flac_get_vtable(void) {
   return &g_flac_decoder_vtable;
}
