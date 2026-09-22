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
 * DAWN Audio Decoder - Registry and Dispatch
 *
 * This module provides the core decoder abstraction:
 * - Extension-based format detection
 * - Vtable dispatch to format-specific implementations
 * - Unified error handling
 */

#include "audio/audio_decoder.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* For strcasecmp */

#include "audio_decoder_internal.h"
#include "logging.h"
#include "utils/string_utils.h"

/* Vtable and base structure defined in audio_decoder_internal.h */

/* =============================================================================
 * Forward Declarations for Format-Specific Decoders
 * ============================================================================= */

/* Each decoder exports a function to get its vtable (single source of truth) */

/* FLAC decoder (always available) */
extern const audio_decoder_vtable_t *flac_get_vtable(void);
extern int flac_get_metadata(const char *path, audio_metadata_t *metadata);

#ifdef DAWN_ENABLE_MP3
/* MP3 decoder (optional, libmpg123) */
extern const audio_decoder_vtable_t *mp3_get_vtable(void);
extern int mp3_decoder_lib_init(void);
extern void mp3_decoder_lib_cleanup(void);
extern int mp3_get_metadata(const char *path, audio_metadata_t *metadata);
#endif

#ifdef DAWN_ENABLE_OGG
/* Ogg Vorbis decoder (optional, libvorbis) */
extern const audio_decoder_vtable_t *ogg_get_vtable(void);
extern int ogg_get_metadata(const char *path, audio_metadata_t *metadata);
#endif

/* =============================================================================
 * Decoder Registry
 * ============================================================================= */

/**
 * @brief Array of all registered decoder vtables
 *
 * Populated at init time from each decoder's get_vtable() function.
 * Order determines priority when multiple decoders could handle a file.
 */
static const audio_decoder_vtable_t *g_decoders[8] = { NULL }; /* Max 7 decoders + NULL */

/* Number of registered decoders (computed at init) */
static size_t g_decoder_count = 0;

/* Initialization flag */
static bool g_initialized = false;

/* Combined extension list for playlist manager */
static const char *g_all_extensions[16] = { NULL }; /* Large enough for all formats */

/* =============================================================================
 * Module State
 * ============================================================================= */

int audio_decoder_init(void) {
   if (g_initialized) {
      OLOG_WARNING("Audio decoder subsystem already initialized");
      return AUDIO_DECODER_SUCCESS;
   }

   /* Initialize format-specific libraries first */
#ifdef DAWN_ENABLE_MP3
   int mp3_result = mp3_decoder_lib_init();
   if (mp3_result != AUDIO_DECODER_SUCCESS) {
      OLOG_WARNING("MP3 decoder initialization failed, MP3 support disabled");
   }
#endif

   /* Build decoder registry from each format's vtable */
   g_decoder_count = 0;
   size_t ext_idx = 0;

   /* FLAC (always available) */
   g_decoders[g_decoder_count++] = flac_get_vtable();

#ifdef DAWN_ENABLE_MP3
   if (mp3_result == AUDIO_DECODER_SUCCESS) {
      g_decoders[g_decoder_count++] = mp3_get_vtable();
   }
#endif

#ifdef DAWN_ENABLE_OGG
   g_decoders[g_decoder_count++] = ogg_get_vtable();
#endif

   g_decoders[g_decoder_count] = NULL; /* Sentinel */

   /* Build combined extension list */
   for (size_t i = 0; g_decoders[i] != NULL; i++) {
      OLOG_INFO("Registered audio decoder: %s", g_decoders[i]->name);

      /* Add all extensions from this decoder */
      for (const char **ext = g_decoders[i]->extensions; *ext != NULL; ext++) {
         if (ext_idx < sizeof(g_all_extensions) / sizeof(g_all_extensions[0]) - 1) {
            g_all_extensions[ext_idx++] = *ext;
         }
      }
   }
   g_all_extensions[ext_idx] = NULL; /* Terminate */

   g_initialized = true;
   OLOG_INFO("Audio decoder initialized: %zu formats, %zu extensions", g_decoder_count, ext_idx);

   return AUDIO_DECODER_SUCCESS;
}

void audio_decoder_cleanup(void) {
   if (!g_initialized) {
      return;
   }

   /* Clean up format-specific libraries */
#ifdef DAWN_ENABLE_MP3
   mp3_decoder_lib_cleanup();
#endif

   /* Clear registry */
   for (size_t i = 0; i < sizeof(g_decoders) / sizeof(g_decoders[0]); i++) {
      g_decoders[i] = NULL;
   }
   g_decoder_count = 0;
   g_initialized = false;

   OLOG_INFO("Audio decoder subsystem cleaned up");
}

/* =============================================================================
 * Extension Detection
 * ============================================================================= */

/**
 * @brief Get file extension from path (including dot)
 *
 * @param path File path
 * @return Pointer to extension (including dot) within path, or NULL if none
 */
static const char *get_extension(const char *path) {
   if (!path) {
      return NULL;
   }

   const char *dot = strrchr(path, '.');
   if (!dot || dot == path) {
      return NULL;
   }

   /* Make sure the dot is after any directory separator */
   const char *slash = strrchr(path, '/');
   if (slash && dot < slash) {
      return NULL;
   }

   return dot;
}

/**
 * @brief Find vtable that handles the given extension
 *
 * @param ext Extension (including dot), case-insensitive
 * @return Matching vtable, or NULL if no match
 */
static const audio_decoder_vtable_t *find_decoder_for_extension(const char *ext) {
   if (!ext) {
      return NULL;
   }

   for (size_t i = 0; g_decoders[i] != NULL; i++) {
      for (const char **dec_ext = g_decoders[i]->extensions; *dec_ext != NULL; dec_ext++) {
         if (strcasecmp(ext, *dec_ext) == 0) {
            return g_decoders[i];
         }
      }
   }

   return NULL;
}

audio_format_type_t audio_decoder_detect_format(const char *path) {
   const char *ext = get_extension(path);
   if (!ext) {
      return AUDIO_FORMAT_UNKNOWN;
   }

   const audio_decoder_vtable_t *vtable = find_decoder_for_extension(ext);
   return vtable ? vtable->format : AUDIO_FORMAT_UNKNOWN;
}

/* =============================================================================
 * Public API Implementation
 * ============================================================================= */

audio_decoder_t *audio_decoder_open(const char *path) {
   if (!g_initialized) {
      OLOG_ERROR("Audio decoder not initialized");
      return NULL;
   }

   if (!path) {
      OLOG_ERROR("audio_decoder_open: NULL path");
      return NULL;
   }

   const char *ext = get_extension(path);
   if (!ext) {
      OLOG_ERROR("audio_decoder_open: No file extension in '%s'", path);
      return NULL;
   }

   const audio_decoder_vtable_t *vtable = find_decoder_for_extension(ext);
   if (!vtable) {
      OLOG_ERROR("audio_decoder_open: Unsupported format '%s'", ext);
      return NULL;
   }

   OLOG_INFO("Opening '%s' with %s decoder", path, vtable->name);
   return vtable->open(path);
}

void audio_decoder_close(audio_decoder_t *dec) {
   if (!dec) {
      return;
   }

   if (dec->vtable && dec->vtable->close) {
      dec->vtable->close(dec);
   }
}

int audio_decoder_get_info(audio_decoder_t *dec, audio_decoder_info_t *info) {
   if (!dec) {
      return AUDIO_DECODER_ERR_INVALID;
   }
   if (!info) {
      return AUDIO_DECODER_ERR_INVALID;
   }
   if (!dec->vtable || !dec->vtable->get_info) {
      return AUDIO_DECODER_ERR_NOT_INIT;
   }

   return dec->vtable->get_info(dec, info);
}

ssize_t audio_decoder_read(audio_decoder_t *dec, int16_t *buffer, size_t max_frames) {
   if (!dec) {
      return -AUDIO_DECODER_ERR_INVALID;
   }
   if (!buffer || max_frames == 0) {
      return -AUDIO_DECODER_ERR_INVALID;
   }
   /* Skip vtable check in hot path - guaranteed valid after open */
   return dec->vtable->read(dec, buffer, max_frames);
}

int audio_decoder_seek(audio_decoder_t *dec, uint64_t sample_pos) {
   if (!dec) {
      return AUDIO_DECODER_ERR_INVALID;
   }
   if (!dec->vtable || !dec->vtable->seek) {
      return AUDIO_DECODER_ERR_SEEK;
   }

   return dec->vtable->seek(dec, sample_pos);
}

int audio_decoder_get_metadata(const char *path, audio_metadata_t *metadata) {
   if (!path || !metadata) {
      return AUDIO_DECODER_ERR_INVALID;
   }

   /* Clear output */
   memset(metadata, 0, sizeof(*metadata));

   if (!g_initialized) {
      OLOG_WARNING("Audio decoder not initialized, cannot get metadata");
      return AUDIO_DECODER_ERR_NOT_INIT;
   }

   /* Detect format and dispatch to format-specific metadata parser */
   audio_format_type_t format = audio_decoder_detect_format(path);

   switch (format) {
      case AUDIO_FORMAT_FLAC:
         return flac_get_metadata(path, metadata);

#ifdef DAWN_ENABLE_MP3
      case AUDIO_FORMAT_MP3:
         return mp3_get_metadata(path, metadata);
#endif

#ifdef DAWN_ENABLE_OGG
      case AUDIO_FORMAT_OGG_VORBIS:
         return ogg_get_metadata(path, metadata);
#endif

      default:
         OLOG_WARNING("No metadata parser for format: %s", audio_decoder_format_name(format));
         return AUDIO_DECODER_ERR_FORMAT;
   }
}

/* =============================================================================
 * Utility Functions
 * ============================================================================= */

const char **audio_decoder_get_extensions(void) {
   return g_all_extensions;
}

const char *audio_decoder_format_name(audio_format_type_t format) {
   switch (format) {
      case AUDIO_FORMAT_FLAC:
         return "FLAC";
      case AUDIO_FORMAT_MP3:
         return "MP3";
      case AUDIO_FORMAT_OGG_VORBIS:
         return "Ogg Vorbis";
      case AUDIO_FORMAT_UNKNOWN:
      default:
         return "Unknown";
   }
}

const char *audio_decoder_error_string(audio_decoder_error_t err) {
   switch (err) {
      case AUDIO_DECODER_SUCCESS:
         return "Success";
      case AUDIO_DECODER_ERR_INVALID:
         return "Invalid parameter or NULL handle";
      case AUDIO_DECODER_ERR_NOT_INIT:
         return "Decoder not initialized";
      case AUDIO_DECODER_ERR_FORMAT:
         return "Unsupported or unknown format";
      case AUDIO_DECODER_ERR_OPEN:
         return "Failed to open file";
      case AUDIO_DECODER_ERR_READ:
         return "Read/decode error";
      case AUDIO_DECODER_ERR_SEEK:
         return "Seek not supported or failed";
      case AUDIO_DECODER_ERR_EOF:
         return "End of file reached";
      case AUDIO_DECODER_ERR_MEMORY:
         return "Memory allocation failed";
      case AUDIO_DECODER_ERR_UNKNOWN:
      default:
         return "Unknown error";
   }
}

bool audio_decoder_format_available(audio_format_type_t format) {
   switch (format) {
      case AUDIO_FORMAT_FLAC:
         return true; /* Always available */
      case AUDIO_FORMAT_MP3:
#ifdef DAWN_ENABLE_MP3
         return true;
#else
         return false;
#endif
      case AUDIO_FORMAT_OGG_VORBIS:
#ifdef DAWN_ENABLE_OGG
         return true;
#else
         return false;
#endif
      default:
         return false;
   }
}
