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
 * Unit tests for music_db.c — search, dedup, browse, source abstraction.
 * Uses a temp-file SQLite database with direct SQL inserts for test data.
 */

#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio/audio_decoder.h"
#include "audio/music_db.h"
#include "audio/music_rank.h"
#include "audio/music_source.h"
#include "dawn_error.h"

/* =============================================================================
 * Stubs for symbols referenced by music_db.c but not exercised in tests
 * ============================================================================= */

/* audio_decoder_get_extensions() — called by is_supported_extension() in scan path only */
static const char *stub_extensions[] = { ".flac", ".mp3", ".ogg", NULL };
const char **audio_decoder_get_extensions(void) {
   return (const char **)stub_extensions;
}

/* audio_decoder_get_metadata() — called during scan, which we never invoke */
int audio_decoder_get_metadata(const char *path, audio_metadata_t *metadata) {
   (void)path;
   (void)metadata;
   return 0;
}

/* =============================================================================
 * Unity Test Framework
 * ============================================================================= */

#include "unity.h"

/* =============================================================================
 * Setup / Teardown
 * ============================================================================= */

static char g_db_path[256];
static sqlite3 *g_test_db = NULL; /* Second handle for inserting test data */

static void setup_db(void) {
   snprintf(g_db_path, sizeof(g_db_path), "/tmp/test_music_db_XXXXXX");
   int fd = mkstemp(g_db_path);
   if (fd < 0) {
      fprintf(stderr, "mkstemp failed\n");
      exit(1);
   }
   close(fd);

   int rc = music_db_init(g_db_path);
   if (rc != 0) {
      fprintf(stderr, "music_db_init failed: %d\n", rc);
      exit(1);
   }

   rc = sqlite3_open(g_db_path, &g_test_db);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "sqlite3_open test handle failed\n");
      exit(1);
   }
   sqlite3_busy_timeout(g_test_db, 5000);
}

static void teardown_db(void) {
   if (g_test_db) {
      sqlite3_close(g_test_db);
      g_test_db = NULL;
   }
   music_db_cleanup();
   unlink(g_db_path);
}

/** Insert a track row directly via the test handle */
static void insert_track(const char *path,
                         const char *title,
                         const char *artist,
                         const char *album,
                         const char *genre,
                         int source,
                         int duration) {
   const char *sql = "INSERT OR REPLACE INTO music_metadata "
                     "(path, mtime, title, artist, album, genre, source, duration_sec) "
                     "VALUES (?, 1000, ?, ?, ?, ?, ?, ?)";
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(g_test_db, sql, -1, &stmt, NULL);
   sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 2, title, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, artist, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 4, album, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 5, genre, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 6, source);
   sqlite3_bind_int(stmt, 7, duration);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
}

/** Clear all rows between test groups */
static void clear_tracks(void) {
   sqlite3_exec(g_test_db, "DELETE FROM music_metadata", NULL, NULL, NULL);
}

/** Free-text search through the ranked query core (strict). */
static void search_text(const char *text, music_search_result_t *results, int max, int *count) {
   music_query_t q = { .text = text };
   *count = 0;
   music_db_query(&q, results, max, count);
}

/* =============================================================================
 * Group 1: music_source abstraction (no DB needed)
 * ============================================================================= */

static void test_source_names(void) {
   printf("\n--- test_source_names ---\n");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(music_source_name(MUSIC_SOURCE_LOCAL), "local") == 0,
                            "LOCAL source name is 'local'");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(music_source_name(MUSIC_SOURCE_PLEX), "plex") == 0,
                            "PLEX source name is 'plex'");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(music_source_name(99), "unknown") == 0,
                            "Invalid source name is 'unknown'");
}

static void test_source_prefixes(void) {
   printf("\n--- test_source_prefixes ---\n");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(music_source_path_prefix(MUSIC_SOURCE_LOCAL), "") == 0,
                            "LOCAL prefix is empty");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(music_source_path_prefix(MUSIC_SOURCE_PLEX), "plex:") == 0,
                            "PLEX prefix is 'plex:'");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(music_source_path_prefix(99), "") == 0,
                            "Invalid source prefix is empty");
}

static void test_source_from_path(void) {
   printf("\n--- test_source_from_path ---\n");
   TEST_ASSERT_TRUE_MESSAGE(music_source_from_path("/home/user/Music/song.flac") ==
                                MUSIC_SOURCE_LOCAL,
                            "Local path identified as LOCAL");
   TEST_ASSERT_TRUE_MESSAGE(music_source_from_path("plex:/library/parts/123/file.mp3") ==
                                MUSIC_SOURCE_PLEX,
                            "plex: path identified as PLEX");
   TEST_ASSERT_TRUE_MESSAGE(music_source_from_path(NULL) == MUSIC_SOURCE_LOCAL,
                            "NULL path returns LOCAL");
}

/* =============================================================================
 * Group 2: Init and schema
 * ============================================================================= */

static void test_init_cleanup(void) {
   printf("\n--- test_init_cleanup ---\n");

   /* Already initialized by setup_db() */
   TEST_ASSERT_TRUE_MESSAGE(music_db_is_initialized() == true, "DB is initialized after init");

   /* Cleanup */
   music_db_cleanup();
   TEST_ASSERT_TRUE_MESSAGE(music_db_is_initialized() == false, "DB not initialized after cleanup");

   /* Double cleanup is safe */
   music_db_cleanup();
   TEST_ASSERT_TRUE_MESSAGE(music_db_is_initialized() == false, "Double cleanup is safe");

   /* Re-init for remaining tests */
   int rc = music_db_init(g_db_path);
   TEST_ASSERT_TRUE_MESSAGE(rc == 0, "Re-init succeeds");
   TEST_ASSERT_TRUE_MESSAGE(music_db_is_initialized() == true, "DB initialized after re-init");
}

static void test_schema_migration_idempotent(void) {
   printf("\n--- test_schema_migration_idempotent ---\n");

   /* Calling init again on same DB should succeed (idempotent) */
   music_db_cleanup();
   int rc = music_db_init(g_db_path);
   TEST_ASSERT_TRUE_MESSAGE(rc == 0, "Second init on same DB succeeds");
   TEST_ASSERT_TRUE_MESSAGE(music_db_is_initialized() == true, "DB initialized after second init");
}

/* =============================================================================
 * Group 3: Search with dedup
 * ============================================================================= */

static void test_search_basic(void) {
   printf("\n--- test_search_basic ---\n");
   clear_tracks();

   insert_track("/music/a.flac", "Song A", "Artist One", "Album X", "Rock", 0, 200);
   insert_track("/music/b.flac", "Song B", "Artist One", "Album X", "Rock", 0, 180);
   insert_track("/music/c.flac", "Song C", "Artist Two", "Album Y", "Jazz", 0, 240);

   music_search_result_t results[10];
   int count = 0;
   search_text("Artist One", results, 10, &count);

   TEST_ASSERT_TRUE_MESSAGE(count == 2, "Search by artist returns 2 matches");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(results[0].artist, "Artist One") == 0,
                               "First result is Artist One");
   }
}

static void test_search_by_title(void) {
   printf("\n--- test_search_by_title ---\n");
   clear_tracks();

   insert_track("/music/a.flac", "Bohemian Rhapsody", "Queen", "Night Opera", "Rock", 0, 355);
   insert_track("/music/b.flac", "Another Song", "Queen", "News", "Rock", 0, 200);

   music_search_result_t results[10];
   int count = 0;
   search_text("Bohemian", results, 10, &count);

   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Search by title finds 1 match");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(results[0].title, "Bohemian Rhapsody") == 0, "Title matches");
   }
}

static void test_search_by_album(void) {
   printf("\n--- test_search_by_album ---\n");
   clear_tracks();

   insert_track("/music/a.flac", "Track 1", "Artist", "Dark Side", "Rock", 0, 300);
   insert_track("/music/b.flac", "Track 2", "Artist", "The Wall", "Rock", 0, 250);

   music_search_result_t results[10];
   int count = 0;
   search_text("Dark Side", results, 10, &count);

   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Search by album finds 1 match");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(results[0].album, "Dark Side") == 0, "Album matches");
   }
}

static void test_search_by_genre(void) {
   printf("\n--- test_search_by_genre ---\n");
   clear_tracks();

   insert_track("/music/a.flac", "Jazz Song", "Miles", "Kind Of Blue", "Jazz", 0, 300);
   insert_track("/music/b.flac", "Rock Song", "Led Zep", "Led Zep IV", "Rock", 0, 250);

   music_search_result_t results[10];
   int count = 0;
   search_text("Jazz", results, 10, &count);

   /* Should match both the genre "Jazz" AND the title "Jazz Song" — but both belong to same track
    */
   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Search by genre finds 1 match");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(results[0].genre, "Jazz") == 0, "Genre matches");
   }
}

static void test_search_dedup_local_wins(void) {
   printf("\n--- test_search_dedup_local_wins ---\n");
   clear_tracks();

   /* Same track on both sources with different case — local (0) should win over plex (1) */
   insert_track("/music/song.flac", "Dedup Song", "Dedup Artist", "Dedup Album", "Rock", 0, 200);
   insert_track("plex:/library/song.mp3", "dedup song", "DEDUP ARTIST", "dedup album", "Rock", 1,
                200);

   music_search_result_t results[10];
   int count = 0;
   search_text("Dedup Song", results, 10, &count);

   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Dedup returns 1 result (not 2)");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(results[0].source == MUSIC_SOURCE_LOCAL, "Local source wins dedup");
      TEST_ASSERT_TRUE_MESSAGE(strncmp(results[0].path, "/music/", 7) == 0, "Local path returned");
   }
}

static void test_search_dedup_plex_only(void) {
   printf("\n--- test_search_dedup_plex_only ---\n");
   clear_tracks();

   /* Track only on Plex — should be returned */
   insert_track("plex:/library/exclusive.mp3", "Plex Only", "Plex Artist", "Plex Album", "Pop", 1,
                180);

   music_search_result_t results[10];
   int count = 0;
   search_text("Plex Only", results, 10, &count);

   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Plex-only track returned");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(results[0].source == MUSIC_SOURCE_PLEX, "Source is PLEX");
   }
}

static void test_search_dedup_case_insensitive(void) {
   printf("\n--- test_search_dedup_case_insensitive ---\n");
   clear_tracks();

   /* Dedup must be case-insensitive: metadata sources often differ in casing */

   /* Artist case difference */
   insert_track("/music/a1.flac", "Song", "Pink Floyd", "Album", "Rock", 0, 200);
   insert_track("plex:/lib/a1.mp3", "Song", "PINK FLOYD", "Album", "Rock", 1, 200);

   music_search_result_t results[10];
   int count = 0;
   search_text("Song", results, 10, &count);
   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Case-diff artist deduplicates to 1");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(results[0].source == MUSIC_SOURCE_LOCAL,
                               "Local wins artist case dedup");
   }

   /* Title case difference */
   clear_tracks();
   insert_track("/music/b1.flac", "Comfortably Numb", "Artist", "Album", "Rock", 0, 300);
   insert_track("plex:/lib/b1.mp3", "comfortably numb", "Artist", "Album", "Rock", 1, 300);

   search_text("Artist", results, 10, &count);
   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Case-diff title deduplicates to 1");

   /* Album case difference */
   clear_tracks();
   insert_track("/music/c1.flac", "Track", "Art", "The Wall", "Rock", 0, 250);
   insert_track("plex:/lib/c1.mp3", "Track", "Art", "THE WALL", "Rock", 1, 250);

   search_text("Track", results, 10, &count);
   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Case-diff album deduplicates to 1");

   /* All three differ in case simultaneously */
   clear_tracks();
   insert_track("/music/d1.flac", "Time", "Pink Floyd", "Dark Side", "Rock", 0, 400);
   insert_track("plex:/lib/d1.mp3", "TIME", "pink floyd", "DARK SIDE", "Rock", 1, 400);

   search_text("Pink Floyd", results, 10, &count);
   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Triple case-diff deduplicates to 1");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(results[0].source == MUSIC_SOURCE_LOCAL,
                               "Local wins triple case dedup");
   }

   /* Stats also reflect case-insensitive dedup */
   music_db_stats_t stats;
   music_db_get_stats(&stats);
   TEST_ASSERT_TRUE_MESSAGE(stats.track_count == 1,
                            "Stats track count reflects case-insensitive dedup");
}

static void test_search_dedup_different_titles(void) {
   printf("\n--- test_search_dedup_different_titles ---\n");
   clear_tracks();

   /* Same artist, different titles — both should survive dedup */
   insert_track("/music/a.flac", "Title Alpha", "Shared Artist", "Album", "Rock", 0, 200);
   insert_track("plex:/lib/b.mp3", "Title Beta", "Shared Artist", "Album", "Rock", 1, 210);

   music_search_result_t results[10];
   int count = 0;
   search_text("Shared Artist", results, 10, &count);

   TEST_ASSERT_TRUE_MESSAGE(count == 2, "Different titles both returned (no false dedup)");
}

static void test_search_like_escaping(void) {
   printf("\n--- test_search_like_escaping ---\n");
   clear_tracks();

   /* "100%" should not act as wildcard — the % must be escaped */
   insert_track("/music/percent.flac", "100% Pure", "Test", "Album", "Rock", 0, 200);
   insert_track("/music/other.flac", "100 reasons", "Test", "Album", "Rock", 0, 180);

   music_search_result_t results[10];
   int count = 0;
   search_text("100% Pure", results, 10, &count);

   /* Should match "100% Pure" but NOT "100 reasons" (% is escaped, not wildcard) */
   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Escaped %% prevents wildcard match");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(results[0].title, "100% Pure") == 0,
                               "Only exact percent match");
   }

   /* "_" should not match single char wildcard */
   clear_tracks();
   insert_track("/music/under.flac", "test_x", "Test", "Album", "Rock", 0, 200);
   insert_track("/music/nope.flac", "testYx", "Test", "Album", "Rock", 0, 180);

   search_text("test_x", results, 10, &count);
   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Escaped _ prevents single-char wildcard");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(results[0].title, "test_x") == 0,
                               "Only literal underscore matches");
   }
}

/* =============================================================================
 * Group 4: Browse with dedup
 * ============================================================================= */

static void test_list_artists_dedup(void) {
   printf("\n--- test_list_artists_dedup ---\n");
   clear_tracks();

   /* Same artist on both sources */
   insert_track("/music/a.flac", "Song A", "Shared Artist", "Album 1", "Rock", 0, 200);
   insert_track("plex:/lib/b.mp3", "Song B", "Shared Artist", "Album 2", "Rock", 1, 210);

   char artists[10][AUDIO_METADATA_STRING_MAX];
   int count = 0;
   music_db_list_artists(artists, 10, 0, &count);

   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Artist appears once despite two sources");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(artists[0], "Shared Artist") == 0, "Artist name correct");
   }
}

static void test_list_albums_dedup(void) {
   printf("\n--- test_list_albums_dedup ---\n");
   clear_tracks();

   /* Same album+artist+title on both sources */
   insert_track("/music/a.flac", "Track 1", "Artist", "Shared Album", "Rock", 0, 200);
   insert_track("plex:/lib/b.mp3", "Track 1", "Artist", "Shared Album", "Rock", 1, 200);

   char albums[10][AUDIO_METADATA_STRING_MAX];
   int count = 0;
   music_db_list_albums(albums, 10, 0, &count);

   TEST_ASSERT_TRUE_MESSAGE(count == 1, "Album appears once despite two sources");
   if (count >= 1) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(albums[0], "Shared Album") == 0, "Album name correct");
   }
}

static void test_get_by_artist_dedup(void) {
   printf("\n--- test_get_by_artist_dedup ---\n");
   clear_tracks();

   /* Duplicate track (same artist+album+title) on both sources */
   insert_track("/music/song.flac", "The Song", "The Artist", "The Album", "Rock", 0, 300);
   insert_track("plex:/lib/song.mp3", "The Song", "The Artist", "The Album", "Rock", 1, 300);
   /* Plus a unique plex track */
   insert_track("plex:/lib/other.mp3", "Other Song", "The Artist", "The Album", "Rock", 1, 250);

   music_search_result_t results[10];
   int count = 0;
   music_db_get_by_artist("The Artist", results, 10, &count);

   TEST_ASSERT_TRUE_MESSAGE(count == 2, "Dedup: 2 unique tracks (not 3 rows)");

   /* Verify the duplicate resolved to local */
   int found_local = 0;
   for (int i = 0; i < count; i++) {
      if (strcmp(results[i].title, "The Song") == 0 && results[i].source == MUSIC_SOURCE_LOCAL)
         found_local = 1;
   }
   TEST_ASSERT_TRUE_MESSAGE(found_local == 1, "Duplicate resolved to local source");
}

static void test_stats_dedup(void) {
   printf("\n--- test_stats_dedup ---\n");
   clear_tracks();

   /* 2 unique tracks, one duplicated across sources = 3 rows, 2 deduped */
   insert_track("/music/a.flac", "Song A", "Artist X", "Album 1", "Rock", 0, 200);
   insert_track("plex:/lib/a.mp3", "Song A", "Artist X", "Album 1", "Rock", 1, 200);
   insert_track("/music/b.flac", "Song B", "Artist Y", "Album 2", "Jazz", 0, 180);

   music_db_stats_t stats;
   int rc = music_db_get_stats(&stats);

   TEST_ASSERT_TRUE_MESSAGE(rc == 0, "get_stats succeeds");
   TEST_ASSERT_TRUE_MESSAGE(stats.track_count == 2, "Deduped track count is 2 (not 3)");
   TEST_ASSERT_TRUE_MESSAGE(stats.artist_count == 2, "Artist count is 2");
   TEST_ASSERT_TRUE_MESSAGE(stats.album_count == 2, "Album count is 2");
}

/* =============================================================================
 * Group 5: Path lookup
 * ============================================================================= */

static void test_get_by_path_local(void) {
   printf("\n--- test_get_by_path_local ---\n");
   clear_tracks();

   insert_track("/music/found.flac", "Found It", "Artist", "Album", "Rock", 0, 200);

   music_search_result_t result;
   bool found = false;
   int rc = music_db_get_by_path("/music/found.flac", &result, &found);

   TEST_ASSERT_TRUE_MESSAGE(rc == 0 && found, "get_by_path finds local track");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(result.title, "Found It") == 0, "Title matches");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(result.path, "/music/found.flac") == 0, "Path matches");
}

static void test_get_by_path_plex(void) {
   printf("\n--- test_get_by_path_plex ---\n");
   clear_tracks();

   insert_track("plex:/library/parts/42/file.mp3", "Plex Track", "Plex Art", "Plex Alb", "Pop", 1,
                300);

   music_search_result_t result;
   bool found = false;
   int rc = music_db_get_by_path("plex:/library/parts/42/file.mp3", &result, &found);

   TEST_ASSERT_TRUE_MESSAGE(rc == 0 && found, "get_by_path finds plex track");
   TEST_ASSERT_TRUE_MESSAGE(strcmp(result.title, "Plex Track") == 0, "Plex title matches");
}

static void test_get_by_path_missing(void) {
   printf("\n--- test_get_by_path_missing ---\n");
   clear_tracks();

   music_search_result_t result;
   bool found = false;
   int rc = music_db_get_by_path("/nonexistent/path.flac", &result, &found);

   TEST_ASSERT_TRUE_MESSAGE(rc == 0 && !found, "get_by_path returns not-found for missing track");
}

/* =============================================================================
 * Group 6: Source-scoped stale deletion
 * ============================================================================= */

static void test_stale_deletion_scoped(void) {
   printf("\n--- test_stale_deletion_scoped ---\n");
   clear_tracks();

   /* Insert local and plex tracks */
   insert_track("/music/keep.flac", "Keep", "Artist", "Album", "Rock", 0, 200);
   insert_track("/music/stale.flac", "Stale", "Artist", "Album", "Rock", 0, 180);
   insert_track("plex:/lib/survive.mp3", "Survive", "Plex Art", "Plex Alb", "Pop", 1, 220);

   /* Simulate stale deletion scoped to local source:
    * Only /music/keep.flac is "seen" — stale.flac should be deleted,
    * but plex track should survive. */
   const char *stale_sql = "CREATE TEMP TABLE IF NOT EXISTS seen_paths (path TEXT PRIMARY KEY)";
   sqlite3_exec(g_test_db, stale_sql, NULL, NULL, NULL);
   sqlite3_exec(g_test_db, "DELETE FROM seen_paths", NULL, NULL, NULL);
   sqlite3_exec(g_test_db, "INSERT INTO seen_paths (path) VALUES ('/music/keep.flac')", NULL, NULL,
                NULL);

   char delete_sql[512];
   snprintf(delete_sql, sizeof(delete_sql),
            "DELETE FROM music_metadata WHERE source = %d "
            "AND path NOT IN (SELECT path FROM seen_paths)",
            MUSIC_SOURCE_LOCAL);
   sqlite3_exec(g_test_db, delete_sql, NULL, NULL, NULL);
   sqlite3_exec(g_test_db, "DROP TABLE seen_paths", NULL, NULL, NULL);

   /* Verify: keep.flac and plex track survive, stale.flac deleted */
   music_search_result_t result;
   bool found = false;

   music_db_get_by_path("/music/keep.flac", &result, &found);
   TEST_ASSERT_TRUE_MESSAGE(found, "Kept local track survives");

   music_db_get_by_path("/music/stale.flac", &result, &found);
   TEST_ASSERT_TRUE_MESSAGE(!found, "Stale local track deleted");

   music_db_get_by_path("plex:/lib/survive.mp3", &result, &found);
   TEST_ASSERT_TRUE_MESSAGE(found, "Plex track survives local stale deletion");
}

/* =============================================================================
 * Main
 * ============================================================================= */

void setUp(void) {
   setup_db();
}

void tearDown(void) {
   teardown_db();
}

/* ============================================================================
 * music_rank_pick_best — pure relevance ranking (no DB)
 * ============================================================================ */

static music_search_result_t mk_result(const char *artist, const char *title) {
   music_search_result_t r;
   memset(&r, 0, sizeof(r));
   snprintf(r.artist, sizeof(r.artist), "%s", artist);
   snprintf(r.title, sizeof(r.title), "%s", title);
   return r;
}

/* The conv-813 regression: bare "Africa" must pick Toto, not Bo Burnham, even
 * though the DB returns Bo Burnham first (alphabetical). */
static void test_pick_best_bare_title_prefers_close_match(void) {
   music_search_result_t r[3] = {
      mk_result("Bo Burnham", "A Prayer / How Do We Fix Africa?"),
      mk_result("Bo Burnham", "A Prayer/How Do We Fix Africa? [Explicit]"),
      mk_result("Toto", "Africa (Single Version)"),
   };
   int pick = music_rank_pick_best(r, 3, "Africa", NULL);
   TEST_ASSERT_EQUAL_INT_MESSAGE(2, pick,
                                 "bare 'Africa' picks Toto (title prefix beats substring)");
}

/* Artist qualifier dominates even when another title is a closer string match. */
static void test_pick_best_artist_dominates(void) {
   music_search_result_t r[2] = {
      mk_result("Bo Burnham", "Africa"),            /* exact title, wrong artist */
      mk_result("Toto", "Africa (Single Version)"), /* prefix title, right artist */
   };
   int pick = music_rank_pick_best(r, 2, "Africa", "Toto");
   TEST_ASSERT_EQUAL_INT_MESSAGE(1, pick, "artist match outweighs a closer title");
}

/* Exact title beats a prefix/substring when no artist is given. */
static void test_pick_best_exact_title(void) {
   music_search_result_t r[2] = {
      mk_result("Spandau Ballet", "Gold (Remastered)"),
      mk_result("Spandau Ballet", "Gold"),
   };
   int pick = music_rank_pick_best(r, 2, "Gold", NULL);
   TEST_ASSERT_EQUAL_INT_MESSAGE(1, pick, "exact title outranks prefix");
}

/* No title match anywhere → falls back to index 0 (does not crash). */
static void test_pick_best_no_match_falls_back(void) {
   music_search_result_t r[2] = {
      mk_result("Artist A", "Totally Unrelated"),
      mk_result("Artist B", "Also Unrelated"),
   };
   int pick = music_rank_pick_best(r, 2, "Nonexistent", NULL);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, pick, "no match falls back to first candidate");
}

/* Defensive: empty candidate set returns 0. */
static void test_pick_best_empty(void) {
   TEST_ASSERT_EQUAL_INT(0, music_rank_pick_best(NULL, 0, "x", NULL));
}

/* =============================================================================
 * Group: ranked, paged structured query (music_db_query_page)
 * ============================================================================= */

/** Insert a track with a release year (and explicit duration). */
static void insert_track_y(const char *path,
                           const char *title,
                           const char *artist,
                           const char *album,
                           int source,
                           int duration,
                           int year) {
   insert_track(path, title, artist, album, "", source, duration);
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(g_test_db, "UPDATE music_metadata SET year = ? WHERE path = ?", -1, &stmt,
                      NULL);
   sqlite3_bind_int(stmt, 1, year);
   sqlite3_bind_text(stmt, 2, path, -1, SQLITE_STATIC);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
}

static music_query_page_t run_page(music_query_t q,
                                   int offset,
                                   music_search_result_t *res,
                                   int max) {
   music_query_page_t pg;
   TEST_ASSERT_EQUAL_INT(SUCCESS, music_db_query_page(&q, offset, res, max, &pg));
   return pg;
}

/* The live failure: the stored title has punctuation the query lacks. */
static void test_query_punctuation_mismatch(void) {
   clear_tracks();
   insert_track("/m/rts.flac", "Rockin' the Suburbs", "Ben Folds", "Rockin' the Suburbs", "", 0,
                300);
   insert_track("/m/dsb.flac", "Don\xE2\x80\x99t Stop Believin'", "Journey", "Escape", "", 0, 250);
   insert_track("/m/acdc.flac", "Back in Black", "AC/DC", "Back in Black", "", 0, 255);
   music_search_result_t res[10];

   music_query_page_t pg = run_page((music_query_t){ .text = "Rockin the Suburbs" }, 0, res, 10);
   TEST_ASSERT_EQUAL_INT(1, pg.total);
   TEST_ASSERT_EQUAL_STRING("/m/rts.flac", res[0].path);

   pg = run_page((music_query_t){ .text = "dont stop believin" }, 0, res, 10);
   TEST_ASSERT_EQUAL_INT(1, pg.total);

   pg = run_page((music_query_t){ .artist = "AC DC" }, 0, res, 10);
   TEST_ASSERT_EQUAL_INT(1, pg.total);
   TEST_ASSERT_FALSE(pg.approximate);
}

/* Pages tile the ranked list exactly: no repeats, no gaps, exact total. */
static void test_query_pagination(void) {
   clear_tracks();
   char path[64];
   char title[64];
   for (int i = 0; i < 30; i++) {
      snprintf(path, sizeof(path), "/m/pager_%02d.flac", i);
      snprintf(title, sizeof(title), "Song %02d", i);
      insert_track(path, title, "Pager", "Paging Album", "", 0, 100 + i);
   }
   music_search_result_t res[10];
   char seen[30][MUSIC_DB_PATH_MAX];
   int nseen = 0;
   for (int page = 0; page < 3; page++) {
      music_query_page_t pg = run_page((music_query_t){ .artist = "Pager" }, page * 10, res, 10);
      TEST_ASSERT_EQUAL_INT(30, pg.total);
      TEST_ASSERT_EQUAL_INT(10, pg.count);
      for (int i = 0; i < pg.count; i++) {
         for (int k = 0; k < nseen; k++) {
            TEST_ASSERT_TRUE_MESSAGE(strcmp(seen[k], res[i].path) != 0, "row repeated");
         }
         snprintf(seen[nseen++], sizeof(seen[0]), "%s", res[i].path);
      }
   }
   TEST_ASSERT_EQUAL_INT(30, nseen);

   /* Past the end: no rows, but the total is still reported. */
   music_query_page_t pg = run_page((music_query_t){ .artist = "Pager" }, 40, res, 10);
   TEST_ASSERT_EQUAL_INT(0, pg.count);
   TEST_ASSERT_EQUAL_INT(30, pg.total);
}

/* The total counts only rows that pass the fielded word-boundary filter. */
static void test_query_total_after_fielded_filter(void) {
   clear_tracks();
   insert_track("/m/p1.flac", "1999", "Prince", "1999", "", 0, 100);
   insert_track("/m/p2.flac", "Kiss", "Prince", "Parade", "", 0, 101);
   insert_track("/m/p3.flac", "Purple Rain", "Prince and the Revolution", "Purple Rain", "", 0,
                102);
   insert_track("/m/n1.flac", "Time", "Ben Folds Presents: The Princeton Nassoons", "UAC", "", 0,
                103);
   insert_track("/m/n2.flac", "Princess", "Someone", "Princess", "", 0, 104);
   music_search_result_t res[10];
   music_query_page_t pg = run_page((music_query_t){ .artist = "Prince" }, 0, res, 10);
   TEST_ASSERT_EQUAL_INT(3, pg.total);
   TEST_ASSERT_EQUAL_STRING("Prince", res[0].artist); /* exact before prefix */

   /* Free text keeps breadth: the mid-word hits count too, ranked below. */
   pg = run_page((music_query_t){ .text = "Prince" }, 0, res, 10);
   TEST_ASSERT_EQUAL_INT(5, pg.total);
   TEST_ASSERT_EQUAL_STRING("Prince", res[0].artist);
}

static void test_query_album_filter(void) {
   clear_tracks();
   insert_track("/m/w1.flac", "Brick", "Ben Folds Five", "Whatever And Ever Amen (Remastered)", "",
                0, 280);
   insert_track("/m/w2.flac", "Kate", "Ben Folds Five", "Whatever and Ever Amen", "", 0, 190);
   insert_track("/m/r1.flac", "Brick", "Ben Folds", "Ben Folds Live", "", 0, 290);
   music_search_result_t res[10];
   music_query_page_t pg = run_page((music_query_t){ .album = "Whatever and Ever Amen" }, 0, res,
                                    10);
   TEST_ASSERT_EQUAL_INT(2, pg.total);
   pg = run_page((music_query_t){ .album = "Whatever and Ever Amen", .title = "Brick" }, 0, res,
                 10);
   TEST_ASSERT_EQUAL_INT(1, pg.total);
   TEST_ASSERT_EQUAL_STRING("/m/w1.flac", res[0].path);
}

/* Ranking covers every match, not an alphabetical window: the one exact hit that
 * sorts last alphabetically still ranks first. */
static void test_query_ranks_whole_match_set(void) {
   clear_tracks();
   char path[64];
   char title[64];
   for (int i = 0; i < 400; i++) {
      snprintf(path, sizeof(path), "/m/aa_%03d.flac", i);
      snprintf(title, sizeof(title), "Love %03d", i);
      insert_track(path, title, "Aardvark", "Alpha", "", 0, i + 1);
   }
   insert_track("/m/zed.flac", "Love", "Zed", "Zulu", "", 0, 999);
   music_search_result_t res[5];
   music_query_page_t pg = run_page((music_query_t){ .text = "Love" }, 0, res, 5);
   TEST_ASSERT_EQUAL_INT(401, pg.total);
   TEST_ASSERT_EQUAL_STRING("Zed", res[0].artist);
}

/* A library indexed twice within one source collapses; distinct same-title
 * tracks (different durations) survive. */
static void test_query_same_source_dedup(void) {
   clear_tracks();
   insert_track("plex:/library/parts/1/a.mp3", "Army", "Ben Folds", "Live", "", 1, 200);
   insert_track("plex:/library/parts/2/a.mp3", "Army", "Ben Folds", "Live", "", 1, 201);
   insert_track("/m/i1.flac", "Interlude", "Band", "Double", "", 0, 60);
   insert_track("/m/i2.flac", "Interlude", "Band", "Double", "", 0, 95);
   music_search_result_t res[10];
   music_query_page_t pg = run_page((music_query_t){ .text = "Army" }, 0, res, 10);
   TEST_ASSERT_EQUAL_INT(1, pg.total);
   TEST_ASSERT_EQUAL_STRING("plex:/library/parts/1/a.mp3", res[0].path); /* earliest copy */
   pg = run_page((music_query_t){ .text = "Interlude" }, 0, res, 10);
   TEST_ASSERT_EQUAL_INT(2, pg.total);
}

static void test_query_cross_field_and_fallback(void) {
   clear_tracks();
   insert_track("/m/c1.flac", "Zak and Sara", "Ben Folds", "Rockin' the Suburbs", "", 0, 200);
   insert_track("/m/c2.flac", "Brick", "Ben Folds Five", "Whatever and Ever Amen", "", 0, 201);
   music_search_result_t res[10];

   /* "Artist Title" free text works across fields, exactly. */
   music_query_page_t pg = run_page((music_query_t){ .text = "Ben Folds Zak" }, 0, res, 10);
   TEST_ASSERT_EQUAL_INT(1, pg.total);
   TEST_ASSERT_FALSE(pg.approximate);

   /* A typo'd long query: no strict match, so the one-word-off rows come back
    * flagged approximate rather than "nothing found". */
   pg = run_page((music_query_t){ .text = "Ben Folds Zak Sarah", .allow_partial = true }, 0, res,
                 10);
   TEST_ASSERT_TRUE(pg.approximate);
   TEST_ASSERT_TRUE(pg.total >= 1);
   TEST_ASSERT_EQUAL_STRING("/m/c1.flac", res[0].path);

   /* Without opting in (play/enqueue/resolve paths), a near-miss is no match. */
   pg = run_page((music_query_t){ .text = "Ben Folds Zak Sarah" }, 0, res, 10);
   TEST_ASSERT_EQUAL_INT(0, pg.total);
   TEST_ASSERT_FALSE(pg.approximate);

   /* When strict matches exist, partials don't pad the result. */
   pg = run_page((music_query_t){ .text = "Ben Folds Brick", .allow_partial = true }, 0, res, 10);
   TEST_ASSERT_EQUAL_INT(1, pg.total);
   TEST_ASSERT_FALSE(pg.approximate);
}

static void test_query_filters_only_and_empty(void) {
   clear_tracks();
   insert_track_y("/m/y1.flac", "A", "X", "Old", 0, 100, 1985);
   insert_track_y("/m/y2.flac", "B", "X", "New", 0, 100, 2005);
   music_search_result_t res[10];
   music_query_page_t pg = run_page((music_query_t){ .year_min = 1980, .year_max = 1989 }, 0, res,
                                    10);
   TEST_ASSERT_EQUAL_INT(1, pg.total);
   TEST_ASSERT_EQUAL_STRING("/m/y1.flac", res[0].path);
   pg = run_page((music_query_t){ 0 }, 0, res, 10);
   TEST_ASSERT_EQUAL_INT(0, pg.total);
   TEST_ASSERT_EQUAL_INT(0, pg.count);
   /* The legacy wrapper still returns the first page. */
   int count = -1;
   music_query_t q = { .text = "Old" };
   TEST_ASSERT_EQUAL_INT(SUCCESS, music_db_query(&q, res, 10, &count));
   TEST_ASSERT_EQUAL_INT(1, count);
}

/* =============================================================================
 * Group: album inventory for an artist
 * ============================================================================= */

static void test_albums_by_artist(void) {
   clear_tracks();
   /* Two editions of one album across sources; one extra bonus track. */
   insert_track_y("/m/rts1.flac", "Zak and Sara", "Ben Folds", "Rockin' the Suburbs", 0, 200, 2001);
   insert_track_y("/m/rts2.flac", "Fired", "Ben Folds", "Rockin' the Suburbs", 0, 210, 2001);
   insert_track_y("plex:/p/1.mp3", "Zak And Sara", "Ben Folds",
                  "Rockin' the Suburbs (Expanded Edition)", 1, 200, 2001);
   insert_track_y("plex:/p/2.mp3", "Bonus Demo", "Ben Folds",
                  "Rockin' the Suburbs (Expanded Edition)", 1, 180, 2001);
   /* Plex indexed twice — must not double the track count. */
   insert_track_y("plex:/p/3.mp3", "Bonus Demo", "Ben Folds",
                  "Rockin' the Suburbs (Expanded Edition)", 1, 180, 2001);
   /* Related act matches as whole words. */
   insert_track_y("/m/w.flac", "Brick", "Ben Folds Five", "Whatever and Ever Amen", 0, 280, 1997);
   /* Compilation credited to many "Ben Folds Presents: X" artists → listed once. */
   insert_track_y("/m/u1.flac", "Time", "Ben Folds Presents: The Princeton Nassoons",
                  "Ben Folds Presents: University A Cappella!", 0, 200, 2009);
   insert_track_y("/m/u2.flac", "Magic", "Ben Folds Presents: The Spartan Dischords",
                  "Ben Folds Presents: University A Cappella!", 0, 201, 2009);
   /* Unrelated artist excluded. */
   insert_track_y("/m/x.flac", "Folds", "Benny Goodman", "Sing Sing Sing", 0, 300, 1938);

   music_album_info_t al[10];
   int count = 0;
   int total = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         music_db_list_albums_by_artist("Ben Folds", al, 10, 0, &count, &total));
   TEST_ASSERT_EQUAL_INT(3, total);
   TEST_ASSERT_EQUAL_INT(3, count);

   /* Oldest first. */
   TEST_ASSERT_EQUAL_STRING("Whatever and Ever Amen", al[0].name);
   TEST_ASSERT_EQUAL_INT(1997, al[0].year);

   TEST_ASSERT_EQUAL_STRING("Rockin' the Suburbs", al[1].name); /* shortest edition name */
   TEST_ASSERT_EQUAL_INT(3, al[1].track_count);                 /* Zak and Sara, Fired, Bonus */
   TEST_ASSERT_EQUAL_INT(2, al[1].editions);
   TEST_ASSERT_EQUAL_INT(1, al[1].artist_count);

   TEST_ASSERT_EQUAL_STRING("Ben Folds Presents: University A Cappella!", al[2].name);
   TEST_ASSERT_EQUAL_INT(2, al[2].artist_count);
   TEST_ASSERT_EQUAL_INT(2, al[2].track_count);

   /* Paging: one per page, past-end still reports the total. */
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         music_db_list_albums_by_artist("Ben Folds", al, 1, 1, &count, &total));
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_INT(3, total);
   TEST_ASSERT_EQUAL_STRING("Rockin' the Suburbs", al[0].name);
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         music_db_list_albums_by_artist("Ben Folds", al, 1, 9, &count, &total));
   TEST_ASSERT_EQUAL_INT(0, count);
   TEST_ASSERT_EQUAL_INT(3, total);

   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         music_db_list_albums_by_artist("Nobody Here", al, 10, 0, &count, &total));
   TEST_ASSERT_EQUAL_INT(0, total);
   TEST_ASSERT_EQUAL_INT(FAILURE, music_db_list_albums_by_artist("", al, 10, 0, &count, &total));
}

/* Review regressions: a same-source copy is never hidden by one a genre/year
 * filter would reject; unknown durations never collapse; fielded filters
 * require every word (filler included). */
static void test_query_review_regressions(void) {
   clear_tracks();
   /* Two same-source copies: only the second is tagged. */
   insert_track("/m/untagged.mp3", "Song", "B", "BA", "", 0, 200);
   insert_track_y("/m/tagged.flac", "Song", "B", "BA", 0, 200, 1999);
   sqlite3_exec(g_test_db, "UPDATE music_metadata SET genre = 'Rock' WHERE path = '/m/tagged.flac'",
                NULL, NULL, NULL);
   music_search_result_t res[10];
   music_query_page_t pg = run_page((music_query_t){ .genre = "rock" }, 0, res, 10);
   TEST_ASSERT_EQUAL_INT(1, pg.total);
   TEST_ASSERT_EQUAL_STRING("/m/tagged.flac", res[0].path);
   pg = run_page((music_query_t){ .year_min = 1990 }, 0, res, 10);
   TEST_ASSERT_EQUAL_INT(1, pg.total);

   /* Cross-source: an untagged local copy must not hide the tagged Plex copy. */
   clear_tracks();
   insert_track("/m/local.flac", "Song", "B", "BA", "", 0, 200);
   insert_track_y("plex:/p/9.flac", "Song", "B", "BA", 1, 200, 1997);
   sqlite3_exec(g_test_db, "UPDATE music_metadata SET genre = 'Rock' WHERE path = 'plex:/p/9.flac'",
                NULL, NULL, NULL);
   pg = run_page((music_query_t){ .genre = "rock" }, 0, res, 10);
   TEST_ASSERT_EQUAL_INT(1, pg.total);
   TEST_ASSERT_EQUAL_STRING("plex:/p/9.flac", res[0].path);
   pg = run_page((music_query_t){ .year_min = 1990 }, 0, res, 10);
   TEST_ASSERT_EQUAL_INT(1, pg.total);
   /* Unfiltered, the local copy still wins as before. */
   pg = run_page((music_query_t){ .text = "Song" }, 0, res, 10);
   TEST_ASSERT_EQUAL_INT(1, pg.total);
   TEST_ASSERT_EQUAL_STRING("/m/local.flac", res[0].path);

   /* Unknown (0) durations: two same-titled tracks both survive. */
   clear_tracks();
   insert_track("/m/i1.flac", "Interlude", "Band", "Double", "", 0, 0);
   insert_track("/m/i2.flac", "Interlude", "Band", "Double", "", 0, 0);
   pg = run_page((music_query_t){ .text = "Interlude" }, 0, res, 10);
   TEST_ASSERT_EQUAL_INT(2, pg.total);

   /* Fielded filters: filler words are required. */
   clear_tracks();
   insert_track("/m/dmb.flac", "Crash", "Dave Matthews Band", "Crash", "", 0, 100);
   insert_track("/m/band.flac", "The Weight", "The Band", "Music from Big Pink", "", 0, 101);
   insert_track("/m/part2.flac", "Part 2", "X", "Y", "", 0, 102);
   pg = run_page((music_query_t){ .artist = "The Band" }, 0, res, 10);
   TEST_ASSERT_EQUAL_INT(1, pg.total);
   TEST_ASSERT_EQUAL_STRING("The Band", res[0].artist);
   pg = run_page((music_query_t){ .title = "Song 2" }, 0, res, 10);
   TEST_ASSERT_EQUAL_INT(0, pg.total);

   /* A wildcard-only genre is not a constraint (no whole-library dump). */
   pg = run_page((music_query_t){ .genre = " * " }, 0, res, 10);
   TEST_ASSERT_EQUAL_INT(0, pg.total);
}

static void test_pick_best_artist_punctuation(void) {
   music_search_result_t r[2] = {
      mk_result("Someone Else", "Back in Black"),
      mk_result("AC/DC", "Back in Black (Live)"),
   };
   TEST_ASSERT_EQUAL_INT(1, music_rank_pick_best(r, 2, "Back in Black", "AC DC"));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_source_names);
   RUN_TEST(test_source_prefixes);
   RUN_TEST(test_source_from_path);
   RUN_TEST(test_init_cleanup);
   RUN_TEST(test_schema_migration_idempotent);
   RUN_TEST(test_search_basic);
   RUN_TEST(test_search_by_title);
   RUN_TEST(test_search_by_album);
   RUN_TEST(test_search_by_genre);
   RUN_TEST(test_search_dedup_local_wins);
   RUN_TEST(test_search_dedup_plex_only);
   RUN_TEST(test_search_dedup_case_insensitive);
   RUN_TEST(test_search_dedup_different_titles);
   RUN_TEST(test_search_like_escaping);
   RUN_TEST(test_list_artists_dedup);
   RUN_TEST(test_list_albums_dedup);
   RUN_TEST(test_get_by_artist_dedup);
   RUN_TEST(test_stats_dedup);
   RUN_TEST(test_get_by_path_local);
   RUN_TEST(test_get_by_path_plex);
   RUN_TEST(test_get_by_path_missing);
   RUN_TEST(test_stale_deletion_scoped);
   RUN_TEST(test_pick_best_bare_title_prefers_close_match);
   RUN_TEST(test_pick_best_artist_dominates);
   RUN_TEST(test_pick_best_exact_title);
   RUN_TEST(test_pick_best_no_match_falls_back);
   RUN_TEST(test_pick_best_empty);
   RUN_TEST(test_query_punctuation_mismatch);
   RUN_TEST(test_query_pagination);
   RUN_TEST(test_query_total_after_fielded_filter);
   RUN_TEST(test_query_album_filter);
   RUN_TEST(test_query_ranks_whole_match_set);
   RUN_TEST(test_query_same_source_dedup);
   RUN_TEST(test_query_cross_field_and_fallback);
   RUN_TEST(test_query_filters_only_and_empty);
   RUN_TEST(test_albums_by_artist);
   RUN_TEST(test_query_review_regressions);
   RUN_TEST(test_pick_best_artist_punctuation);
   return UNITY_END();
}
