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
 * Music Source Abstraction
 *
 * Defines source identifiers, priority, and the provider interface for
 * pluggable music library sync modules.
 *
 * Enum value IS the priority: lower value = higher priority.
 * When duplicate tracks exist (same artist + album + title),
 * the source with the lowest enum value wins.
 *
 * To add a new source:
 *   1. Add enum value here (before MUSIC_SOURCE_COUNT)
 *   2. Add name string to music_source_name() in music_source.c
 *   3. Add path prefix to music_source_path_prefix() if needed
 *   4. Create sync module (e.g., subsonic_db.c) implementing music_source_provider_t
 *   5. Register provider in dawn.c via music_scanner_register_source()
 */

#ifndef MUSIC_SOURCE_H
#define MUSIC_SOURCE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Source Identifiers
 * ============================================================================= */

typedef enum {
   MUSIC_SOURCE_LOCAL = 0, /* Highest priority - local filesystem */
   MUSIC_SOURCE_PLEX = 1,  /* Plex Media Server */
   /* MUSIC_SOURCE_SUBSONIC = 2, */
   /* MUSIC_SOURCE_JELLYFIN = 3, */
   MUSIC_SOURCE_COUNT
} music_source_t;

/* =============================================================================
 * Source Utilities
 * ============================================================================= */

/** Name for logging/display (e.g., "local", "plex").  It is also the word a
 *  query names the source with ("source:plex"), so keep it one lowercase token. */
const char *music_source_name(music_source_t source);

/** Path prefix used to identify tracks from this source (e.g., "plex:") */
const char *music_source_path_prefix(music_source_t source);

/** Identify source from a track path (returns MUSIC_SOURCE_LOCAL if no prefix) */
music_source_t music_source_from_path(const char *path);

/* =============================================================================
 * Sync Provider Interface
 * ============================================================================= */

/**
 * Sync provider interface - implemented by each remote source module.
 * Local scan is handled internally by music_db_scan() and is not a provider.
 */
typedef struct {
   music_source_t source;
   int (*init)(const char *db_path);    /* Open own DB handle, create state */
   void (*cleanup)(void);               /* Close DB handle, free state */
   int (*sync)(void);                   /* Run one sync cycle */
   bool (*is_configured)(void);         /* Return true if source is available */
   bool (*initial_sync_complete)(void); /* Has first sync finished? */
} music_source_provider_t;

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_SOURCE_H */
