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
 * DAWN Audio Decoder Abstraction
 *
 * Provides a unified interface for decoding multiple audio formats (FLAC, MP3, Ogg Vorbis).
 * Uses opaque handle pattern consistent with audio_backend.h.
 *
 * Supported Formats:
 *   - FLAC (always available via libFLAC)
 *   - MP3 (optional, via libmpg123 if DAWN_ENABLE_MP3 defined)
 *   - Ogg Vorbis (optional, via libvorbis if DAWN_ENABLE_OGG defined)
 *
 * Usage:
 *   1. Call audio_decoder_init() at startup
 *   2. Open files with audio_decoder_open() (auto-detects format by extension)
 *   3. Get metadata with audio_decoder_get_info()
 *   4. Read samples with audio_decoder_read() in a loop
 *   5. Close with audio_decoder_close()
 *   6. Call audio_decoder_cleanup() at shutdown
 *
 * Thread Safety:
 *   - audio_decoder_init()/cleanup() are NOT thread-safe
 *   - Handle operations are thread-safe for different handles
 *   - Same handle should not be used from multiple threads concurrently
 */

#ifndef AUDIO_DECODER_H
#define AUDIO_DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* For ssize_t */

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Error Codes (positive values per DAWN coding standards)
 * ============================================================================= */

/**
 * @brief Audio decoder error codes
 *
 * All error returns use positive values. Functions returning ssize_t use
 * negative values to indicate errors (the actual error code is the absolute
 * value). Functions returning int use 0 for success and positive values for
 * specific errors.
 */
typedef enum {
   AUDIO_DECODER_SUCCESS = 0,      /**< Operation successful */
   AUDIO_DECODER_ERR_INVALID = 1,  /**< Invalid parameter or NULL handle */
   AUDIO_DECODER_ERR_NOT_INIT = 2, /**< Decoder subsystem not initialized */
   AUDIO_DECODER_ERR_FORMAT = 3,   /**< Unsupported or unknown format */
   AUDIO_DECODER_ERR_OPEN = 4,     /**< Failed to open file */
   AUDIO_DECODER_ERR_READ = 5,     /**< Read/decode error */
   AUDIO_DECODER_ERR_SEEK = 6,     /**< Seek not supported or failed */
   AUDIO_DECODER_ERR_EOF = 7,      /**< End of file reached */
   AUDIO_DECODER_ERR_MEMORY = 8,   /**< Memory allocation failed */
   AUDIO_DECODER_ERR_UNKNOWN = 9   /**< Unknown or unrecoverable error */
} audio_decoder_error_t;

/* =============================================================================
 * Format Types
 * ============================================================================= */

/**
 * @brief Audio format types
 */
typedef enum {
   AUDIO_FORMAT_UNKNOWN = 0, /**< Unknown or unsupported format */
   AUDIO_FORMAT_FLAC,        /**< FLAC (Free Lossless Audio Codec) */
   AUDIO_FORMAT_MP3,         /**< MP3 (MPEG Audio Layer III) */
   AUDIO_FORMAT_OGG_VORBIS   /**< Ogg Vorbis */
} audio_format_type_t;

/* =============================================================================
 * Opaque Handle Type
 * ============================================================================= */

/**
 * @brief Opaque handle for audio decoder
 *
 * Internally contains vtable pointer for format-specific dispatch.
 * Use audio_decoder_open() to create and audio_decoder_close() to destroy.
 */
typedef struct audio_decoder audio_decoder_t;

/* =============================================================================
 * Decoder Metadata
 * ============================================================================= */

/**
 * @brief Audio file information/metadata
 *
 * Retrieved via audio_decoder_get_info() after opening a file.
 */
typedef struct {
   uint32_t sample_rate;       /**< Sample rate in Hz (e.g., 44100, 48000) */
   uint8_t channels;           /**< Number of channels (1=mono, 2=stereo) */
   uint8_t bits_per_sample;    /**< Bits per sample (typically 16 or 24) */
   uint64_t total_samples;     /**< Total samples (per channel), 0 if unknown */
   audio_format_type_t format; /**< Detected audio format */
} audio_decoder_info_t;

/* =============================================================================
 * Tag Metadata (Artist/Title/Album)
 * ============================================================================= */

/** Maximum length for metadata string fields (artist, title, album) */
#define AUDIO_METADATA_STRING_MAX 256

/** Maximum length for genre string (genres are short: "Rock", "Electronic", etc.) */
#define AUDIO_GENRE_STRING_MAX 128

/**
 * @brief Audio file tag metadata (ID3, Vorbis comments, etc.)
 *
 * Retrieved via audio_decoder_get_metadata() - separate from stream info
 * because metadata parsing may be slower and not always needed.
 */
typedef struct {
   char title[AUDIO_METADATA_STRING_MAX];  /**< Track title (e.g., "Time") */
   char artist[AUDIO_METADATA_STRING_MAX]; /**< Artist name (e.g., "Pink Floyd") */
   char album[AUDIO_METADATA_STRING_MAX];  /**< Album name (e.g., "Dark Side of the Moon") */
   char genre[AUDIO_GENRE_STRING_MAX];     /**< Genre (e.g., "Rock"); comma-joined if multiple */
   uint32_t year;                          /**< Release year (0 if unknown) */
   uint32_t duration_sec;                  /**< Duration in seconds (0 if unknown) */
   bool has_metadata;                      /**< True if any tag metadata was found */
} audio_metadata_t;

/* =============================================================================
 * Initialization / Cleanup
 * ============================================================================= */

/**
 * @brief Initialize the audio decoder subsystem
 *
 * Must be called before any other audio_decoder_* functions.
 * Registers all available format decoders (FLAC, MP3, Ogg).
 * NOT thread-safe; call from main thread before creating other threads.
 *
 * @return AUDIO_DECODER_SUCCESS on success, or error code on failure
 */
int audio_decoder_init(void);

/**
 * @brief Clean up the audio decoder subsystem
 *
 * Releases global resources. Does NOT close open handles.
 * NOT thread-safe; call from main thread after closing all handles.
 */
void audio_decoder_cleanup(void);

/* =============================================================================
 * File Operations
 * ============================================================================= */

/**
 * @brief Open an audio file for decoding
 *
 * Auto-detects format based on file extension (case-insensitive).
 * Supported: .flac, .mp3, .ogg
 *
 * @param path Path to audio file
 * @return Decoder handle on success, NULL on error
 */
audio_decoder_t *audio_decoder_open(const char *path);

/**
 * @brief Close decoder and release resources
 *
 * Safe to call with NULL (no-op).
 *
 * @param dec Decoder handle
 */
void audio_decoder_close(audio_decoder_t *dec);

/**
 * @brief Get audio file metadata
 *
 * Retrieves sample rate, channels, total samples, and format info.
 *
 * @param dec Decoder handle
 * @param info Output: Audio information structure
 * @return AUDIO_DECODER_SUCCESS on success, or error code
 */
int audio_decoder_get_info(audio_decoder_t *dec, audio_decoder_info_t *info);

/**
 * @brief Read decoded audio samples
 *
 * Reads interleaved 16-bit signed samples. For stereo, samples are interleaved
 * as L0, R0, L1, R1, ... A "frame" is one sample per channel.
 *
 * @param dec Decoder handle
 * @param buffer Output buffer for samples (interleaved int16_t)
 * @param max_frames Maximum frames to read
 * @return Number of frames read (0 at EOF), or negative error code (-AUDIO_DECODER_ERR_*)
 */
ssize_t audio_decoder_read(audio_decoder_t *dec, int16_t *buffer, size_t max_frames);

/**
 * @brief Seek to a sample position
 *
 * Not all formats/files support seeking. Returns AUDIO_DECODER_ERR_SEEK if
 * seeking is not supported.
 *
 * @param dec Decoder handle
 * @param sample_pos Target sample position (per channel)
 * @return AUDIO_DECODER_SUCCESS on success, or error code
 */
int audio_decoder_seek(audio_decoder_t *dec, uint64_t sample_pos);

/**
 * @brief Get audio file tag metadata (artist, title, album)
 *
 * Extracts embedded tags from audio files:
 *   - FLAC: Vorbis comments
 *   - Ogg Vorbis: Vorbis comments
 *   - MP3: ID3v1 and ID3v2 tags
 *
 * This function opens and closes the file independently of any decoder handle,
 * making it suitable for batch metadata scanning.
 *
 * @param path Path to audio file
 * @param metadata Output structure (will be zeroed on failure)
 * @return AUDIO_DECODER_SUCCESS on success, or error code
 */
int audio_decoder_get_metadata(const char *path, audio_metadata_t *metadata);

/**
 * @brief Parse a 4-digit release year from a tag date string
 *
 * Handles the common tag forms ("1985", "1985-06-01", "1985/06", "06/1985") by
 * returning the first standalone run of four digits within a plausible range.
 *
 * @param s Date/year tag value (may be NULL)
 * @return Year in [1000, 2999], or 0 if none found
 */
uint32_t audio_metadata_parse_year(const char *s);

/**
 * @brief Append a genre value to a metadata struct, comma-joining if non-empty
 *
 * Formats stored as multiple genre entries (repeated Vorbis GENRE=) are
 * flattened into one comma-separated string for parity with the Plex path.
 * Bounded to AUDIO_GENRE_STRING_MAX.
 *
 * @param metadata Target metadata (genre field appended in place)
 * @param genre    Genre value to add (ignored if NULL/empty)
 */
void audio_metadata_append_genre(audio_metadata_t *metadata, const char *genre);

/* =============================================================================
 * Utility Functions
 * ============================================================================= */

/**
 * @brief Detect audio format from file extension
 *
 * Case-insensitive extension matching.
 *
 * @param path File path
 * @return Format type, or AUDIO_FORMAT_UNKNOWN if not recognized
 */
audio_format_type_t audio_decoder_detect_format(const char *path);

/**
 * @brief Get list of supported file extensions
 *
 * Returns NULL-terminated array of extension strings (including dot).
 * Array is static; do not free.
 *
 * @return Array of extensions, e.g., {".flac", ".mp3", ".ogg", NULL}
 */
const char **audio_decoder_get_extensions(void);

/**
 * @brief Get human-readable format name
 *
 * @param format Format type
 * @return Format name string (e.g., "FLAC", "MP3", "Ogg Vorbis")
 */
const char *audio_decoder_format_name(audio_format_type_t format);

/**
 * @brief Get error message for error code
 *
 * @param err Error code from decoder operation
 * @return Human-readable error string
 */
const char *audio_decoder_error_string(audio_decoder_error_t err);

/**
 * @brief Check if a format is available
 *
 * Some formats (MP3, Ogg) are conditionally compiled.
 *
 * @param format Format type to check
 * @return true if format is available, false otherwise
 */
bool audio_decoder_format_available(audio_format_type_t format);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_DECODER_H */
