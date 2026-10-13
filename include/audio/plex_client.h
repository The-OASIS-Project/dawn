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
 * Plex Media Server REST API client for music library browsing and streaming
 */

#ifndef PLEX_CLIENT_H
#define PLEX_CLIENT_H

#include <json-c/json.h>
#include <stdatomic.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

/** Max capacity for Plex API JSON responses (8MB — some Plex servers ignore
 * Container-Size pagination and return all items in a single response) */
#define PLEX_API_MAX_RESPONSE (8 * 1024 * 1024)

/** Max download size for audio files (500MB — covers 24-bit/96kHz FLAC) */
#define PLEX_DOWNLOAD_MAX_SIZE (500LL * 1024 * 1024)

/** Temp file prefix for downloaded tracks.
 * Uses /var/tmp/ (disk-backed) instead of /tmp (often tmpfs/RAM on Jetson)
 * to avoid consuming unified GPU/CPU memory for large audio files. */
#define PLEX_TEMP_PREFIX "/var/tmp/dawn_plex_"

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

/**
 * @brief Initialize the Plex client
 *
 * Creates persistent CURL handles for API queries and file downloads.
 * Cleans up any orphaned temp files from previous runs.
 *
 * @return 0 on success, non-zero on error
 */
int plex_client_init(void);

/**
 * @brief Clean up Plex client resources
 *
 * Frees CURL handles and any cached state.
 */
void plex_client_cleanup(void);

/**
 * @brief Check if the Plex client is configured and ready
 *
 * @return true if host and token are configured
 */
bool plex_client_is_configured(void);

/* =============================================================================
 * Section Discovery
 * ============================================================================= */

/**
 * @brief Discover the music library section ID
 *
 * Calls GET /library/sections and finds the first section with type="artist".
 * If music_section_id is configured (non-zero), uses that directly.
 *
 * @param section_id_out Output: discovered section ID
 * @return 0 on success, non-zero on error
 */
int plex_client_discover_section(int *section_id_out);

/* =============================================================================
 * Library Browsing
 * ============================================================================= */

/**
 * @brief List all tracks in the music library (paginated)
 *
 * Used by plex_db sync to fetch all tracks for indexing into the unified DB.
 *
 * @param offset Pagination offset
 * @param limit  Max results to return
 * @return JSON object in music_library_response format, or NULL
 */
json_object *plex_client_list_all_tracks(int offset, int limit);

/* =============================================================================
 * Library Change Detection
 * ============================================================================= */

/**
 * @brief Get the library's last-scanned timestamp for change detection
 *
 * Cheap single API call to check if the library has changed since last sync.
 *
 * @param updated_at_out Output: scannedAt timestamp from Plex
 * @return 0 on success, non-zero on error
 */
int plex_client_get_library_updated_at(time_t *updated_at_out);

/* =============================================================================
 * Streaming
 * ============================================================================= */

/** plex_client_download_track() result: the Plex server couldn't be reached */
#define PLEX_DOWNLOAD_UNREACHABLE 2

/**
 * @brief Download a Plex track to a temporary file
 *
 * Constructs the full URL with token, downloads to /tmp/dawn_plex_XXXXXX.ext,
 * and sets permissions to 0600. The temp file is unlinked immediately after
 * the caller opens it (Unix fd trick).
 *
 * @param part_key The Part.key from Plex API (e.g., "/library/parts/9877/.../file.flac")
 * @param cancel Optional flag; once set, the download aborts within about a second and the
 *               call fails.  NULL = not cancellable
 * @param out_path Output: path to the downloaded temp file
 * @param out_path_size Size of out_path buffer
 * @return SUCCESS, PLEX_DOWNLOAD_UNREACHABLE if the server couldn't be reached, or
 *         FAILURE on any other error
 */
int plex_client_download_track(const char *part_key,
                               const atomic_bool *cancel,
                               char *out_path,
                               size_t out_path_size);

/* =============================================================================
 * Playback Reporting
 * ============================================================================= */

/**
 * @brief Report a track as fully played (scrobble)
 *
 * @param rating_key The Plex ratingKey for the track
 * @return 0 on success, non-zero on error
 */
int plex_client_scrobble(const char *rating_key);

/* =============================================================================
 * Connection Testing
 * ============================================================================= */

/**
 * @brief Test connection to the Plex server
 *
 * Calls GET /identity to verify server is reachable and token is valid.
 *
 * @param server_name_out Output buffer for server friendly name (may be NULL)
 * @param name_size Size of server_name_out buffer
 * @return 0 on success, non-zero on error
 */
int plex_client_test_connection(char *server_name_out, size_t name_size);

/**
 * @brief Clean up orphaned temp files from previous runs
 *
 * Called during init to remove any /tmp/dawn_plex_* files left from crashes.
 */
void plex_client_cleanup_temp_files(void);

#ifdef __cplusplus
}
#endif

#endif /* PLEX_CLIENT_H */
