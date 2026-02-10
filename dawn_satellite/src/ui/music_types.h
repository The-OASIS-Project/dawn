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
 * Music data types shared between ws_client (JSON parsing) and ui_music (rendering).
 * This header has NO SDL dependencies so the network layer stays decoupled from UI.
 */

#ifndef MUSIC_TYPES_H
#define MUSIC_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define MUSIC_MAX_TITLE 256
#define MUSIC_MAX_ARTIST 256
#define MUSIC_MAX_ALBUM 256
#define MUSIC_MAX_PATH 1024
#define MUSIC_MAX_QUEUE 100
#define MUSIC_MAX_RESULTS 50

/* =============================================================================
 * Data Types
 * ============================================================================= */

typedef struct {
   char path[MUSIC_MAX_PATH];
   char title[MUSIC_MAX_TITLE];
   char artist[MUSIC_MAX_ARTIST];
   char album[MUSIC_MAX_ALBUM];
   uint32_t duration_sec;
} music_track_t;

typedef enum {
   MUSIC_TAB_PLAYING,
   MUSIC_TAB_QUEUE,
   MUSIC_TAB_LIBRARY,
} music_tab_t;

typedef enum {
   MUSIC_BROWSE_NONE,
   MUSIC_BROWSE_TRACKS,
   MUSIC_BROWSE_ARTISTS,
   MUSIC_BROWSE_ALBUMS,
   MUSIC_BROWSE_BY_ARTIST,
   MUSIC_BROWSE_BY_ALBUM,
} music_browse_type_t;

typedef struct {
   char name[MUSIC_MAX_TITLE];
   int track_count;
   int album_count; /* artists only */
} music_browse_item_t;

/* Typed state struct passed from ws_client -> ui_music */
typedef struct {
   bool playing;
   bool paused;
   music_track_t track;
   float duration_sec;
   char source_format[16];
   int source_rate;
   int bitrate;
   char bitrate_mode[8];
} music_state_update_t;

typedef struct {
   music_track_t tracks[MUSIC_MAX_QUEUE];
   int count;
   int current_index;
} music_queue_update_t;

typedef struct {
   int stat_tracks;
   int stat_artists;
   int stat_albums;
   music_browse_type_t browse_type;
   music_browse_item_t items[MUSIC_MAX_RESULTS];
   int item_count;
   music_track_t tracks[MUSIC_MAX_RESULTS];
   int track_count;
   int total_count; /* Total tracks in DB (for pagination) */
   int offset;      /* Offset of this page */
} music_library_update_t;

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_TYPES_H */
