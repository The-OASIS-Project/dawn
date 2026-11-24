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
 * the project author(s).
 *
 * The music tool's actions over a music player: whichever player the caller
 * has (a WebUI tab's, the local speaker's).  Its listener hears what changed
 * through the player's output (music_player_notify_state/_error) and other
 * players through the queue's listener (music_queue_announce).  Runs on the
 * caller's thread: an LLM tool worker, or the main voice loop for a direct
 * command, which must never wait long (tool_start_track).
 */

#define _GNU_SOURCE /* strcasestr */

#include "tools/music_player_tool.h"

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "audio/music_db.h"
#include "audio/music_open.h"
#include "audio/music_player.h"
#include "audio/music_queue.h"
#include "audio/music_rank.h"
#include "core/strbuf.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/music_search.h"
#include "tools/tool_registry.h"
#include "utils/string_utils.h"

/* "Artist - Title" display string size for resolved items. */
#define MUSIC_TOOL_DISPLAY_MAX (MUSIC_STRING_MAX * 2)

/** Albums listed when album: matches several */
#define MUSIC_TOOL_ALBUM_CHOICES 10

/** How long an LLM tool call waits for its track to open: past the Plex
 *  connect timeout, so "unreachable" and "won't open" are answered truthfully,
 *  without leaving a voice turn silent for a slow download */
#define MUSIC_TOOL_OPEN_WAIT_MS 15000
/** The wait anywhere else (a direct voice command runs on the main loop,
 *  which must keep hearing the microphone): long enough for a local file */
#define MUSIC_TOOL_SHORT_WAIT_MS 250

/* =============================================================================
 * LLM-facing helpers: state footer, item resolution, batch apply + report
 * ============================================================================= */

/**
 * @brief Append a compact "[queue: ... | now ... | shuffle | repeat]" footer
 *
 * Gives the LLM the resulting player state after every mutating action so it
 * never has to call list to stay oriented.  Only for the model: a direct
 * voice command would speak it.
 */
static void append_state_footer(strbuf_t *sb, music_player_t *player, bool for_model) {
   if (!for_model) {
      return;
   }
   music_player_status_t st;
   music_player_snapshot(player, &st);
   int len = st.queue_length;
   bool shuffle = st.shuffle;
   music_repeat_mode_t rep = st.repeat_mode;
   int idx = st.queue_index;
   bool now = st.track.path[0] != '\0';
   const char *now_title = st.track.title;
   const char *now_artist = st.track.artist;

   const char *rep_s = rep == MUSIC_REPEAT_ALL ? "all" : (rep == MUSIC_REPEAT_ONE ? "one" : "none");
   if (len > 0 && now && idx < 0) {
      /* Another tab replaced the queue; this session plays on a track it no longer holds */
      strbuf_appendf(sb,
                     "\n[queue: %d | now \"%s\" — %s, not in the queue | shuffle %s | repeat %s]",
                     len, now_title[0] ? now_title : "Unknown",
                     now_artist[0] ? now_artist : "Unknown", shuffle ? "on" : "off", rep_s);
   } else if (len > 0) {
      strbuf_appendf(sb, "\n[queue: %d | now #%d \"%s\" — %s | shuffle %s | repeat %s]", len,
                     idx + 1, now_title[0] ? now_title : "Unknown",
                     now_artist[0] ? now_artist : "Unknown", shuffle ? "on" : "off", rep_s);
   } else if (now) {
      strbuf_appendf(
          sb, "\n[queue: empty | now \"%s\" — %s, not in the queue | shuffle %s | repeat %s]",
          now_title[0] ? now_title : "Unknown", now_artist[0] ? now_artist : "Unknown",
          shuffle ? "on" : "off", rep_s);
   } else {
      strbuf_appendf(sb, "\n[queue: empty | shuffle %s | repeat %s]", shuffle ? "on" : "off",
                     rep_s);
   }
}

/* How long a call waits for its track to open */
static int open_wait_ms(bool for_model) {
   return for_model ? MUSIC_TOOL_OPEN_WAIT_MS : MUSIC_TOOL_SHORT_WAIT_MS;
}

/*
 * Start a track and wait for the player's verdict, so the reply is true.
 * The state event goes out first (playing, and loading for a remote track)
 * so the listener shows the change at once; the player reports the outcome
 * itself.  Only a call for the model waits for a slow open.
 */
static music_open_result_t tool_start_track(music_player_t *player,
                                            const char *path,
                                            int hint,
                                            bool for_model) {
   uint32_t ticket = 0;
   if (music_player_play(player, path, hint, &ticket) != SUCCESS) {
      music_player_notify_error(player, MUSIC_ERROR_PLAYBACK, "Failed to start playback");
      music_player_notify_state(player);
      return MUSIC_OPEN_FAILED;
   }
   music_player_notify_state(player);
   return music_player_await(player, ticket, open_wait_ms(for_model));
}

/* How an outcome reads after a track's name ("X is playing", "X couldn't...") */
static const char *tool_outcome_phrase(music_open_result_t result) {
   switch (result) {
      case MUSIC_OPEN_PLAYING:
         return "is playing";
      case MUSIC_OPEN_UNREACHABLE:
         return "can't play: the Plex server isn't reachable";
      case MUSIC_OPEN_PENDING:
         return "is still loading and starts when it's ready";
      case MUSIC_OPEN_PREEMPTED:
         return "was still loading when another music request came in, which may change it";
      case MUSIC_OPEN_CANCELLED:
         return "was replaced before it started";
      case MUSIC_OPEN_CLOSED:
         return "didn't start: the player closed";
      case MUSIC_OPEN_FAILED:
      default:
         return "couldn't be played";
   }
}

/* A track that couldn't play fails the call; one still loading doesn't */
static int tool_outcome_rc(music_open_result_t result) {
   return result == MUSIC_OPEN_FAILED || result == MUSIC_OPEN_UNREACHABLE ||
                  result == MUSIC_OPEN_CLOSED
              ? FAILURE
              : SUCCESS;
}

/** strdup(msg) with the state footer appended; falls back to plain msg on OOM. */
static char *result_with_footer(music_player_t *player, const char *msg, bool for_model) {
   strbuf_t sb;
   strbuf_init(&sb, 256);
   strbuf_appendf(&sb, "%s", msg);
   append_state_footer(&sb, player, for_model);
   if (strbuf_oom(&sb)) {
      strbuf_free(&sb);
      return strdup(msg);
   }
   return strbuf_steal(&sb);
}

/**
 * @brief Resolve a free-text track name or library path to a playable path
 *
 * Accepts an absolute library path (exact lookup) or a name (best match within
 * @p f, NULL for the whole library, see music_search_resolve_item()). Returns
 * SUCCESS and fills path_out (+ optional display).
 */
static int resolve_item_to_path(const char *item,
                                const music_search_filters_t *f,
                                char *path_out,
                                size_t path_len,
                                char *display_out,
                                size_t display_len) {
   if (!item || !item[0]) {
      return FAILURE;
   }

   music_search_result_t r;

   /* Exact library path → exact lookup. music_path_is_valid() accepts both
    * local absolute paths and validated plex: paths (the forms 'search' returns),
    * and rejects free-text names — so names fall through to the ranked search
    * below. Doubles as defense in depth (no out-of-library file). */
   if (music_path_is_valid(item)) {
      bool found = false;
      if (music_db_get_by_path(item, &r, &found) != SUCCESS || !found) {
         return FAILURE;
      }
   } else if (music_search_resolve_item(item, f, &r) != SUCCESS) {
      return FAILURE;
   }

   safe_strncpy(path_out, r.path, path_len);
   if (display_out) {
      /* Format into a full-size buffer (can't truncate → no
       * -Wformat-truncation), then copy-truncate into the caller's. */
      char label[2 * AUDIO_METADATA_STRING_MAX + 4];
      snprintf(label, sizeof(label), "%s - %s", r.artist, r.title);
      safe_strncpy(display_out, label, display_len);
   }
   return SUCCESS;
}

/**
 * @brief Decode the ::items:: field into a JSON string array
 *
 * Returns a json_type_array the caller must json_object_put(), or NULL if
 * absent / not an array.
 */
static struct json_object *decode_array(const char *value, const char *field) {
   if (!value || !value[0]) {
      return NULL;
   }
   size_t cap = strlen(value) + 1;
   char *json = malloc(cap);
   if (!json) {
      return NULL;
   }
   bool got = tool_param_extract_custom(value, field, json, cap);
   struct json_object *arr = got ? json_tokener_parse(json) : NULL;
   free(json);
   if (arr && json_object_is_type(arr, json_type_array)) {
      return arr;
   }
   if (arr) {
      json_object_put(arr);
   }
   return NULL;
}

/**
 * @brief Resolve a JSON array of item names/paths and apply them to the queue
 *
 * Resolves each entry, calls music_queue_apply_paths(), and builds a
 * grouped "Added / No match" report naming the items so the
 * LLM can self-correct. Caller adds the state footer and handles playback start.
 *
 * @param replace        true → replace the queue (play); false → append (enqueue)
 * @param added_count_out optional: number of tracks actually added
 * @return allocated report string (caller frees), or NULL on OOM
 */
static char *apply_items_and_report(music_player_t *player,
                                    struct json_object *arr,
                                    bool replace,
                                    int *added_count_out) {
   int requested = (int)json_object_array_length(arr);
   int n = requested;
   if (added_count_out) {
      *added_count_out = 0;
   }
   if (n <= 0) {
      return strdup("No items provided.");
   }
   int dropped_over_cap = 0;
   if (n > MUSIC_QUEUE_MAX) {
      dropped_over_cap = n - MUSIC_QUEUE_MAX;
      n = MUSIC_QUEUE_MAX;
   }

   char(*paths)[MUSIC_PATH_MAX] = calloc((size_t)n, sizeof(*paths));
   char(*disp)[MUSIC_TOOL_DISPLAY_MAX] = calloc((size_t)n, sizeof(*disp));
   const char **pptr = calloc((size_t)n, sizeof(*pptr));
   music_queue_outcome_t *oc = calloc((size_t)n, sizeof(*oc));
   if (!paths || !disp || !pptr || !oc) {
      free(paths);
      free(disp);
      free(pptr);
      free(oc);
      OLOG_ERROR("apply_items_and_report: allocation failed for %d items", n);
      return NULL;
   }

   strbuf_t nomatch;
   strbuf_init(&nomatch, 128);
   int k = 0; /* count of resolved items */
   for (int i = 0; i < n; i++) {
      struct json_object *e = json_object_array_get_idx(arr, i);
      const char *item = e ? json_object_get_string(e) : NULL;
      if (resolve_item_to_path(item, NULL, paths[k], sizeof(paths[k]), disp[k], sizeof(disp[k])) ==
          SUCCESS) {
         pptr[k] = paths[k];
         k++;
      } else if (item && item[0]) {
         strbuf_appendf(&nomatch, "%s%s", nomatch.len ? ", " : "", item);
      }
   }

   /* Replacing with nothing would empty the queue for a list that matched
    * nothing: leave it (and what's playing) as it is.  A failed apply left
    * the queue untouched, and its outcomes unset: nothing was added. */
   bool applied = true;
   if (k > 0 || !replace) {
      applied = music_queue_apply_paths(music_player_queue(player), pptr, k, replace, oc) ==
                SUCCESS;
   }

   strbuf_t added;
   strbuf_init(&added, 256);
   int added_count = 0;
   for (int j = 0; applied && j < k; j++) {
      if (oc[j] == MUSIC_QUEUE_ADDED) {
         strbuf_appendf(&added, "%s%s", added.len ? ", " : "", disp[j]);
         added_count++;
      } else if (oc[j] == MUSIC_QUEUE_FULL) {
         strbuf_appendf(&nomatch, "%s%s (queue full)", nomatch.len ? ", " : "", disp[j]);
      }
   }

   strbuf_t out;
   strbuf_init(&out, 512);
   strbuf_appendf(&out, "Added %d track%s.", added_count, added_count == 1 ? "" : "s");
   if (added.len) {
      strbuf_appendf(&out, " Matched: %s.", added.buf);
   }
   if (nomatch.len) {
      strbuf_appendf(&out, " No match: %s.", nomatch.buf);
   }
   if (dropped_over_cap > 0) {
      strbuf_appendf(&out, " %d item%s beyond the %d-track queue limit were skipped.",
                     dropped_over_cap, dropped_over_cap == 1 ? "" : "s", MUSIC_QUEUE_MAX);
   }
   if (!applied) {
      strbuf_appendf(&out, " The queue couldn't be updated.");
   }

   if (added_count_out) {
      *added_count_out = added_count;
   }

   char *result = strbuf_oom(&out) ? NULL : strbuf_steal(&out);
   if (!result) {
      strbuf_free(&out);
   }
   strbuf_free(&nomatch);
   strbuf_free(&added);
   free(paths);
   free(disp);
   free(pptr);
   free(oc);
   return result;
}


static int music_tool_locked(music_player_t *player,
                             const char *action,
                             const char *query,
                             bool for_model,
                             char **result_out);

/* A bare play or a resume: resume a pause, else start the queue at the
 * player's place when nothing is playing */
static int tool_resume(music_player_t *player,
                       bool resume_only,
                       bool for_model,
                       char **result_out) {
   uint32_t ticket = 0;
   music_resume_t r = music_player_resume_or_start(player, &ticket);
   const char *msg = NULL;
   char started[96];
   int rc = SUCCESS;
   switch (r) {
      case MUSIC_RESUME_RESUMED:
         msg = "Resumed playback";
         break;
      case MUSIC_RESUME_STARTED: {
         /* The queue starts at the player's place: answer with its outcome */
         music_player_notify_state(player);
         music_open_result_t outcome = music_player_await(player, ticket, open_wait_ms(for_model));
         snprintf(started, sizeof(started), "%sThe queue's track %s",
                  tool_outcome_rc(outcome) == SUCCESS ? "" : TOOL_RESULT_ERROR_MARK,
                  tool_outcome_phrase(outcome));
         if (result_out) {
            *result_out = result_with_footer(player, started, for_model);
         }
         return tool_outcome_rc(outcome);
      }
      case MUSIC_RESUME_START_FAILED:
         music_player_notify_error(player, MUSIC_ERROR_PLAYBACK, "Failed to start playback");
         msg = TOOL_RESULT_ERROR_MARK "Couldn't start playback";
         rc = FAILURE;
         break;
      case MUSIC_RESUME_UNCHANGED:
      default: {
         music_player_status_t st;
         music_player_snapshot(player, &st);
         if (st.playing && !st.paused) {
            msg = "Already playing";
         } else {
            msg = resume_only ? TOOL_RESULT_ERROR_MARK "Nothing to resume: the queue is empty"
                              : TOOL_RESULT_ERROR_MARK "The queue is empty: say what to play, or "
                                                       "pass items:[...] for a track list";
            rc = FAILURE;
         }
         break;
      }
   }
   if (rc == SUCCESS) {
      music_player_notify_state(player);
   }
   if (result_out) {
      *result_out = result_with_footer(player, msg, for_model);
   }
   return rc;
}

/* "[id N] 'Name' by Artist, M tracks (1-M)" for a choice list */
/* A tag for one line of the list: control characters (a newline that would start a
 * line of its own) as spaces */
static void choice_text(const char *in, char *out, size_t cap) {
   safe_strncpy(out, in, cap);
   for (char *p = out; *p; p++) {
      if ((unsigned char)*p < 0x20 || *p == 0x7f) {
         *p = ' ';
      }
   }
}

static void append_choice(strbuf_t *sb, const music_album_choice_t *c) {
   char name[sizeof(c->name)];
   char artist[sizeof(c->artist)];
   choice_text(c->name, name, sizeof(name));
   choice_text(c->artist[0] ? c->artist : "Unknown", artist, sizeof(artist));
   strbuf_appendf(sb, "\n[id %lld] '%s' by %s, %d track%s", (long long)c->id, name, artist,
                  c->track_count, c->track_count == 1 ? "" : "s");
   if (c->first_track > 0) {
      strbuf_appendf(sb, " (%d-%d)", c->first_track, c->last_track);
   }
}

/*
 * The album copy album: (artist: narrowing) names: the only album found, or the
 * only one named exactly ("Iron Man" over "Iron Man 2").  Otherwise 0, with the
 * reply in @p sb: none found, or the choices with their ids.
 */
static int64_t pick_album(const music_search_filters_t *f, strbuf_t *sb) {
   music_album_choice_t choices[MUSIC_TOOL_ALBUM_CHOICES];
   int n = 0;
   int total = 0;
   const char *artist = f->artist[0] ? f->artist : NULL;
   if (music_db_album_find(f->album, artist, choices, MUSIC_TOOL_ALBUM_CHOICES, 0, &n, &total) !=
       SUCCESS) {
      strbuf_appendf(sb, TOOL_RESULT_ERROR_MARK "Couldn't search the music library.");
      return 0;
   }
   if (n == 1 || (n > 1 && choices[0].exact && !choices[1].exact)) {
      return choices[0].id;
   }
   if (n == 0) {
      strbuf_appendf(sb, TOOL_RESULT_ERROR_MARK "No album matching '%s'%s%s.", f->album,
                     artist ? " by " : "", artist ? artist : "");
      return 0;
   }
   strbuf_appendf(sb, TOOL_RESULT_ERROR_MARK "album:'%s' matches %d albums:", f->album, total);
   for (int i = 0; i < n; i++) {
      append_choice(sb, &choices[i]);
   }
   if (total > n) {
      strbuf_appendf(sb, "\n...and %d more (artist: narrows it)", total - n);
   }
   strbuf_appendf(sb, "\nNothing changed. Play the one meant with album_ids:[id] (several "
                      "play in order).");
   return 0;
}

/*
 * The tracks to queue for an album request: the copies album_ids names, in
 * order, else the album album: names.  On failure @p sb holds the reply.
 */
static int album_tracks(const music_search_filters_t *f,
                        struct json_object *ids,
                        music_search_result_t *out,
                        int *count_out,
                        strbuf_t *sb) {
   int count = 0;
   size_t n_ids = ids ? json_object_array_length(ids) : 0;
   for (size_t i = 0; ids && i < n_ids && count < MUSIC_SEARCH_ALBUM_ROWS_MAX; i++) {
      int64_t id = json_object_get_int64(json_object_array_get_idx(ids, i));
      int got = 0;
      if (id <= 0 ||
          music_db_album_tracks(id, out + count, MUSIC_SEARCH_ALBUM_ROWS_MAX - count, &got) !=
              SUCCESS ||
          got == 0) {
         strbuf_appendf(sb,
                        TOOL_RESULT_ERROR_MARK "Album id %lld isn't in the library (ids change "
                                               "when the library is rescanned; look it up "
                                               "again with album:). Nothing changed.",
                        (long long)id);
         return FAILURE;
      }
      count += got;
   }
   if (!ids) {
      int64_t id = pick_album(f, sb);
      if (id == 0) {
         return FAILURE;
      }
      if (music_db_album_tracks(id, out, MUSIC_SEARCH_ALBUM_ROWS_MAX, &count) != SUCCESS) {
         strbuf_appendf(sb, TOOL_RESULT_ERROR_MARK "Couldn't read that album. Nothing changed.");
         return FAILURE;
      }
   }
   if (count == 0) {
      strbuf_appendf(sb, TOOL_RESULT_ERROR_MARK "That album has no tracks. Nothing changed.");
      return FAILURE;
   }
   *count_out = count;
   return SUCCESS;
}

/* Whether @p q scores at least @p min against some track's album name and artist */
static bool names_album(const music_search_result_t *tracks,
                        int count,
                        const char *artist,
                        const char *q,
                        int min) {
   char named[AUDIO_METADATA_STRING_MAX * 2 + 2];
   for (int i = 0; i < count; i++) {
      snprintf(named, sizeof(named), "%s %s", tracks[i].album,
               artist && artist[0] ? artist : tracks[i].artist);
      if (music_rank_field_quality(named, q) >= min) {
         return true;
      }
   }
   return false;
}

/*
 * Where albums start for a query given with them: track 1 when there is none or
 * every word of it is in an album's name and artist ("Abbey Road by The
 * Beatles"); else the track whose title holds all its words ("Here Come the
 * Sun" starts "Here Comes the Sun"; a word may be part of a title word, but not
 * misspelled).  False when a query names neither: what was meant isn't clear,
 * so the caller changes nothing and says what it found.
 */
static bool album_start(const music_search_result_t *tracks,
                        int count,
                        const char *title,
                        const char *artist,
                        int *start_out) {
   *start_out = 0;
   if (!title || !title[0] || names_album(tracks, count, artist, title, MUSIC_RANK_FIELD_MIN)) {
      return true;
   }
   int best = 0;
   for (int i = 0; i < count; i++) {
      int q = music_rank_field_quality(tracks[i].title, title);
      if (q > best) {
         best = q;
         *start_out = i;
      }
   }
   return best > 0;
}

/* play / enqueue album:'X': the whole album, replacing the queue (and
 * playing it) or appended */
static int album_to_queue(music_player_t *player,
                          const music_search_filters_t *f,
                          struct json_object *ids,
                          const char *title,
                          bool replace,
                          bool for_model,
                          char **result_out) {
   if (!music_db_is_initialized()) {
      if (result_out)
         *result_out = strdup("Music database not available");
      return FAILURE;
   }
   /* The whole album, so the disc asked for is chosen from all of it; the queue
    * takes what fits */
   music_search_result_t *tracks = malloc(MUSIC_SEARCH_ALBUM_ROWS_MAX * sizeof(*tracks));
   if (!tracks) {
      if (result_out)
         *result_out = strdup("Memory allocation failed");
      return FAILURE;
   }
   strbuf_t reply;
   strbuf_init(&reply, 512);
   int count = 0;
   if (album_tracks(f, ids, tracks, &count, &reply) != SUCCESS) {
      if (result_out) {
         *result_out = strbuf_oom(&reply) ? strdup(TOOL_RESULT_ERROR_MARK "No album played")
                                          : strbuf_steal(&reply);
      }
      strbuf_free(&reply);
      free(tracks);
      return FAILURE;
   }
   strbuf_free(&reply);
   int start = 0;
   int discs = 1;
   count = music_search_album_disc(tracks, count, ids ? NULL : f->album, f->artist, &discs);
   char album[AUDIO_METADATA_STRING_MAX + 48];
   size_t n_ids = ids ? json_object_array_length(ids) : 0;
   if (n_ids > 1) {
      snprintf(album, sizeof(album), "%zu albums", n_ids);
   } else {
      char set[AUDIO_METADATA_STRING_MAX];
      char name[AUDIO_METADATA_STRING_MAX];
      if (discs > 1) {
         music_search_album_set_name(tracks[0].album, set, sizeof(set));
      } else {
         snprintf(set, sizeof(set), "%s", tracks[0].album);
      }
      choice_text(set, name, sizeof(name));
      if (discs > 1) {
         snprintf(album, sizeof(album), "the album '%s' (all %d discs)", name, discs);
      } else {
         snprintf(album, sizeof(album), "the album '%s'", name);
      }
   }
   /* A start is a track the queue holds */
   int fits = count < MUSIC_QUEUE_MAX ? count : MUSIC_QUEUE_MAX;
   if (!album_start(tracks, fits, title, f->artist, &start)) {
      if (result_out) {
         strbuf_t sb;
         strbuf_init(&sb, 512);
         strbuf_appendf(&sb,
                        TOOL_RESULT_ERROR_MARK
                        "Found %s, but no track title on it matches '%s'. Nothing changed. To "
                        "play it from track 1, play without a query; to find the song, search "
                        "for it.",
                        album, title);
         *result_out = strbuf_oom(&sb) ? strdup(TOOL_RESULT_ERROR_MARK "No track matched")
                                       : strbuf_steal(&sb);
         strbuf_free(&sb);
      }
      free(tracks);
      return FAILURE;
   }

   user_music_queue_t *uq = music_player_queue(player);
   char first_title[MUSIC_STRING_MAX];
   char first_path[MUSIC_PATH_MAX];
   safe_strncpy(first_title, tracks[start].title, sizeof(first_title));
   safe_strncpy(first_path, tracks[start].path, sizeof(first_path));
   int added = 0;
   if (replace) {
      music_player_stop(player);
   }
   music_queue_add(uq, tracks, count, replace, NULL, &added);
   free(tracks);
   music_queue_announce(uq, player);

   if (!replace) {
      music_player_notify_state(player);
      if (result_out) {
         char buf[AUDIO_METADATA_STRING_MAX + 160];
         snprintf(buf, sizeof(buf), "%sAdded %d track%s from %s%s.",
                  added == 0 ? TOOL_RESULT_ERROR_MARK : "", added, added == 1 ? "" : "s", album,
                  added < count ? " (the queue is full)" : "");
         *result_out = result_with_footer(player, buf, for_model);
      }
      return added > 0 ? SUCCESS : FAILURE;
   }
   music_open_result_t outcome = tool_start_track(player, first_path, start, for_model);
   if (result_out) {
      char over[48] = "";
      char buf[AUDIO_METADATA_STRING_MAX + MUSIC_STRING_MAX + sizeof(over) + 200];
      if (added < count) {
         snprintf(over, sizeof(over), "; %d more didn't fit the queue", count - added);
      }
      snprintf(buf, sizeof(buf), "Replaced queue with %s (%d track%s%s); #%d, %s, %s.", album,
               added, added == 1 ? "" : "s", over, start + 1,
               first_title[0] ? first_title : "Unknown", tool_outcome_phrase(outcome));
      *result_out = result_with_footer(player, buf, for_model);
   }
   return tool_outcome_rc(outcome);
}

int music_player_tool_execute(music_player_t *player,
                              const char *action,
                              const char *query,
                              bool for_model,
                              char **result_out) {
   if (!player || !action) {
      return FAILURE;
   }

   /* A direct command (the voice loop runs it) preempts: a model call waiting
    * for its track to open steps aside rather than hold up the voice loop.  A
    * model call waits its turn behind other controls. */
   if (!music_player_control_lock(player, !for_model)) {
      if (result_out) {
         *result_out = strdup(TOOL_RESULT_ERROR_MARK "That player has closed; nothing changed");
      }
      return FAILURE;
   }
   int ret = music_tool_locked(player, action, query, for_model, result_out);
   music_player_control_unlock(player);
   return ret;
}

/* music_player_tool_execute's body, with the player's control lock held */
static int music_tool_locked(music_player_t *player,
                             const char *action,
                             const char *query,
                             bool for_model,
                             char **result_out) {
   OLOG_INFO("Music tool: action='%s' query='%s'", action, query ? query : "(none)");

   /* The bridge passes the packed value string; the real search query is the
    * base segment, and list params arrive in the ::items:: field. */
   char base_query[MUSIC_PATH_MAX] = { 0 };
   tool_param_extract_base(query ? query : "", base_query, sizeof(base_query));
   user_music_queue_t *uq = music_player_queue(player);

   if (strcmp(action, "play") == 0) {
      /* items[] → REPLACE the queue with an explicit, resolved track list */
      struct json_object *items = decode_array(query, "items");
      if (items) {
         /* Resolve first: a list that matches nothing leaves the queue and
          * what's playing alone */
         int added = 0;
         char *report = apply_items_and_report(player, items, true, &added);
         json_object_put(items);

         music_open_result_t outcome = MUSIC_OPEN_PLAYING;
         if (added > 0) {
            music_player_stop(player);
            char first_path[MUSIC_PATH_MAX] = { 0 };
            pthread_mutex_lock(&uq->queue_mutex);
            if (uq->queue_length > 0) {
               snprintf(first_path, sizeof(first_path), "%s", uq->queue[0].path);
            }
            pthread_mutex_unlock(&uq->queue_mutex);
            music_queue_announce(uq, player);
            outcome = first_path[0] ? tool_start_track(player, first_path, 0, for_model)
                                    : MUSIC_OPEN_FAILED;
         }

         if (result_out) {
            strbuf_t sb;
            strbuf_init(&sb, 512);
            if (added > 0) {
               strbuf_appendf(&sb, "Replaced queue. %s The first track %s.", report ? report : "",
                              tool_outcome_phrase(outcome));
               if (for_model) {
                  strbuf_appendf(&sb, " Use 'enqueue' to add without clearing.");
               }
            } else {
               /* Marked: nothing changed, as for the local speaker */
               strbuf_appendf(&sb,
                              TOOL_RESULT_ERROR_MARK "%s The queue and playback are unchanged.",
                              report ? report : "");
            }
            append_state_footer(&sb, player, for_model);
            if (strbuf_oom(&sb)) {
               strbuf_free(&sb);
               *result_out = strdup("Replaced queue");
            } else {
               *result_out = strbuf_steal(&sb);
            }
         }
         free(report);
         return tool_outcome_rc(outcome);
      }

      music_search_filters_t filters;
      music_search_parse_filters(query, &filters);
      struct json_object *ids = decode_array(query, "album_ids");
      if (ids && json_object_array_length(ids) == 0) {
         json_object_put(ids);
         ids = NULL;
      }
      if (ids || filters.album[0]) {
         int rc = album_to_queue(player, &filters, ids, base_query, true, for_model, result_out);
         json_object_put(ids);
         return rc;
      }

      if (base_query[0] == '\0' && !music_search_filters_any(&filters)) {
         /* A bare play: resume a pause, or start the queue at the player's
          * place; with nothing to start, ask for something */
         return tool_resume(player, false, for_model, result_out);
      }

      /* Search unified database and play (REPLACE).  Search first: a miss must
       * leave what's playing alone. */
      if (!music_db_is_initialized()) {
         if (result_out)
            *result_out = strdup("Music database not available");
         return FAILURE;
      }
      music_search_result_t *results = malloc(MUSIC_QUEUE_MAX * sizeof(music_search_result_t));
      if (!results) {
         if (result_out)
            *result_out = strdup("Memory allocation failed");
         return FAILURE;
      }
      int count = 0;
      music_query_t mq;
      music_search_build_query(&mq, base_query, &filters);
      if (music_db_query(&mq, results, MUSIC_QUEUE_MAX, &count) != SUCCESS || count <= 0) {
         free(results);
         if (result_out) {
            strbuf_t sb;
            strbuf_init(&sb, 128);
            strbuf_append(&sb, TOOL_RESULT_ERROR_MARK "No music found for ");
            music_search_describe_query(&sb, base_query, &filters);
            *result_out = strbuf_oom(&sb) ? NULL : strbuf_steal(&sb);
            if (!*result_out) {
               strbuf_free(&sb);
            }
         }
         return FAILURE;
      }

      music_player_stop(player);

      char first_title[MUSIC_STRING_MAX] = { 0 };
      char first_path[MUSIC_PATH_MAX] = { 0 };
      int total_queued = 0;

      music_queue_add(uq, results, count, true, NULL, &total_queued);
      safe_strncpy(first_path, results[0].path, sizeof(first_path));
      safe_strncpy(first_title, results[0].title, sizeof(first_title)); /* cut as queued */
      free(results);

      /* Start playback of first track */
      music_queue_announce(uq, player);
      music_open_result_t outcome = tool_start_track(player, first_path, 0, for_model);

      if (result_out) {
         strbuf_t sb;
         strbuf_init(&sb, 256);
         strbuf_appendf(&sb, "Replaced queue with %d track%s matching ", total_queued,
                        total_queued == 1 ? "" : "s");
         music_search_describe_query(&sb, base_query, &filters);
         strbuf_appendf(&sb, "; #1, %s, %s.%s", first_title[0] ? first_title : "Unknown",
                        tool_outcome_phrase(outcome),
                        for_model ? " (Use 'enqueue' to add without clearing.)" : "");
         *result_out = result_with_footer(player, strbuf_oom(&sb) ? "Replaced queue" : sb.buf,
                                          for_model);
         strbuf_free(&sb);
      }
      return tool_outcome_rc(outcome);

   } else if (strcmp(action, "enqueue") == 0) {
      /* APPEND to the queue — items[] (preferred) or a single query (top match). */
      struct json_object *items = decode_array(query, "items");
      music_search_filters_t filters;
      music_search_parse_filters(query, &filters);
      struct json_object *ids = items ? NULL : decode_array(query, "album_ids");
      if (ids && json_object_array_length(ids) == 0) {
         json_object_put(ids);
         ids = NULL;
      }
      /* With a query, enqueue adds that one track, found within the filters */
      if (ids || (!items && filters.album[0] && base_query[0] == '\0')) {
         int rc = album_to_queue(player, &filters, ids, NULL, false, for_model, result_out);
         json_object_put(ids);
         return rc;
      }
      if (!items && base_query[0] == '\0') {
         if (result_out)
            *result_out = strdup("enqueue needs items:[...], album:'...', album_ids:[...] or a "
                                 "query");
         return FAILURE;
      }

      char *report = NULL;
      if (items) {
         report = apply_items_and_report(player, items, false, NULL);
         json_object_put(items);
      } else {
         char path[MUSIC_PATH_MAX] = { 0 };
         char disp[MUSIC_TOOL_DISPLAY_MAX] = { 0 };
         bool filtered = music_search_filters_any(&filters);
         if (resolve_item_to_path(base_query, filtered ? &filters : NULL, path, sizeof(path), disp,
                                  sizeof(disp)) == SUCCESS) {
            const char *p = path;
            music_queue_outcome_t oc = MUSIC_QUEUE_NOT_FOUND;
            music_queue_apply_paths(uq, &p, 1, false, &oc);
            char buf[MUSIC_TOOL_DISPLAY_MAX + 64];
            if (oc == MUSIC_QUEUE_ADDED)
               snprintf(buf, sizeof(buf), "Added %s.", disp);
            else if (oc == MUSIC_QUEUE_FULL)
               snprintf(buf, sizeof(buf), "Could not add %s (queue full).", disp);
            else
               snprintf(buf, sizeof(buf), "Could not add %s.", disp);
            report = strdup(buf);
         } else if (filtered) {
            /* Nothing added: never a track from outside what the call named */
            if (result_out) {
               strbuf_t sb;
               strbuf_init(&sb, 256);
               strbuf_append(&sb, TOOL_RESULT_ERROR_MARK "No track matches ");
               music_search_describe_query(&sb, base_query, &filters);
               strbuf_append(&sb, ". Nothing was added.");
               if (filters.album[0]) {
                  strbuf_append(&sb, " Without a query, album: adds the whole album.");
               }
               append_state_footer(&sb, player, for_model);
               *result_out = strbuf_oom(&sb) ? NULL : strbuf_steal(&sb);
               if (!*result_out) {
                  strbuf_free(&sb);
               }
            }
            return FAILURE;
         } else {
            char buf[MUSIC_PATH_MAX + 64];
            snprintf(buf, sizeof(buf), "No match: %s.", base_query);
            report = strdup(buf);
         }
      }

      /* enqueue only adds; it never starts playback (play, next or select
       * does) */
      music_player_notify_state(player);
      music_queue_announce(uq, player);

      if (result_out) {
         *result_out = result_with_footer(player, report ? report : "Nothing added.", for_model);
      }
      free(report);
      return SUCCESS;

   } else if (strcmp(action, "clear") == 0) {
      int removed = 0;
      music_player_clear_queue(player, &removed);
      music_player_notify_state(player);
      music_queue_announce(uq, player);
      if (result_out) {
         char buf[128];
         snprintf(buf, sizeof(buf), "Cleared queue (was %d track%s).", removed,
                  removed == 1 ? "" : "s");
         *result_out = result_with_footer(player, buf, for_model);
      }
      return SUCCESS;

   } else if (strcmp(action, "remove") == 0) {
      /* 1-based index, or a substring match against queue titles. */
      if (base_query[0] == '\0') {
         if (result_out)
            *result_out = strdup("remove needs a track number (1-based) or a title to match");
         return FAILURE;
      }

      int index0 = -1;
      char *endp = NULL;
      long num = strtol(base_query, &endp, 10);
      if (endp && *endp == '\0') {
         index0 = (int)num - 1; /* 1-based → 0-based */
      } else {
         /* Substring match against artist/title; first match wins. */
         pthread_mutex_lock(&uq->queue_mutex);
         for (int i = 0; i < uq->queue_length; i++) {
            if (strcasestr(uq->queue[i].title, base_query) ||
                strcasestr(uq->queue[i].artist, base_query)) {
               index0 = i;
               break;
            }
         }
         pthread_mutex_unlock(&uq->queue_mutex);
         if (index0 < 0) {
            if (result_out) {
               char buf[MUSIC_PATH_MAX + 64];
               snprintf(buf, sizeof(buf), "No queued track matching '%s'", base_query);
               *result_out = strdup(buf);
            }
            return FAILURE;
         }
      }

      bool start_failed = false;
      int removed_rc = music_player_remove(player, index0, &start_failed);
      if (start_failed) {
         music_player_notify_error(player, MUSIC_ERROR_PLAYBACK, "Failed to start playback");
      }
      if (removed_rc != SUCCESS) {
         int len;
         pthread_mutex_lock(&uq->queue_mutex);
         len = uq->queue_length;
         pthread_mutex_unlock(&uq->queue_mutex);
         if (result_out) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Invalid track number. Choose 1-%d", len);
            *result_out = strdup(buf);
         }
         return FAILURE;
      }
      music_player_notify_state(player);
      music_queue_announce(uq, player);
      if (result_out) {
         char buf[64];
         snprintf(buf, sizeof(buf), "Removed track %d.", index0 + 1);
         *result_out = result_with_footer(player, buf, for_model);
      }
      return SUCCESS;

   } else if (strcmp(action, "stop") == 0) {
      music_player_stop(player);
      music_player_notify_state(player);

      if (result_out) {
         *result_out = result_with_footer(player, "Music playback stopped", for_model);
      }
      return SUCCESS;

   } else if (strcmp(action, "pause") == 0) {
      if (!music_player_set_paused(player, true)) {
         music_player_status_t st;
         music_player_snapshot(player, &st);
         if (result_out) {
            *result_out = result_with_footer(
                player, st.paused ? "Already paused" : TOOL_RESULT_ERROR_MARK "Nothing is playing",
                for_model);
         }
         return st.paused ? SUCCESS : FAILURE;
      }
      music_player_notify_state(player);

      if (result_out) {
         *result_out = result_with_footer(player, "Music paused", for_model);
      }
      return SUCCESS;

   } else if (strcmp(action, "resume") == 0) {
      return tool_resume(player, true, for_model, result_out);

   } else if (strcmp(action, "select") == 0) {
      /* Play the queue's track N (1-based) */
      char *endp = NULL;
      long num = strtol(base_query, &endp, 10);
      char path[MUSIC_PATH_MAX] = { 0 };
      char title[MUSIC_STRING_MAX] = { 0 };
      int len = 0;
      pthread_mutex_lock(&uq->queue_mutex);
      len = uq->queue_length;
      bool valid = base_query[0] && endp && *endp == '\0' && num >= 1 && num <= len;
      if (valid) {
         snprintf(path, sizeof(path), "%s", uq->queue[num - 1].path);
         snprintf(title, sizeof(title), "%s", uq->queue[num - 1].title);
      }
      pthread_mutex_unlock(&uq->queue_mutex);
      if (!valid) {
         if (result_out) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     len > 0 ? TOOL_RESULT_ERROR_MARK "Invalid track number. Choose 1-%d"
                             : TOOL_RESULT_ERROR_MARK "The queue is empty",
                     len);
            *result_out = strdup(buf);
         }
         return FAILURE;
      }
      music_open_result_t outcome = tool_start_track(player, path, (int)num - 1, for_model);
      if (result_out) {
         char buf[MUSIC_STRING_MAX + 96];
         snprintf(buf, sizeof(buf), "Track %ld, %s, %s.", num, title[0] ? title : "Unknown",
                  tool_outcome_phrase(outcome));
         *result_out = result_with_footer(player, buf, for_model);
      }
      return tool_outcome_rc(outcome);

   } else if (strcmp(action, "next") == 0 || strcmp(action, "previous") == 0) {
      bool next = strcmp(action, "next") == 0;
      char path[MUSIC_PATH_MAX];
      char title[MUSIC_STRING_MAX];
      if (!music_player_step(player, next ? MUSIC_NAV_NEXT : MUSIC_NAV_PREVIOUS, path, sizeof(path),
                             title, sizeof(title))) {
         if (result_out) {
            *result_out = strdup(next ? "Already at end of queue"
                                      : "Nothing earlier to go back to");
         }
         return SUCCESS;
      }

      music_open_result_t outcome = tool_start_track(player, path, -1, for_model);

      if (result_out) {
         char buf[384];
         snprintf(buf, sizeof(buf), "%s track, %s, %s.", next ? "Next" : "Previous",
                  title[0] ? title : "Unknown", tool_outcome_phrase(outcome));
         *result_out = result_with_footer(player, buf, for_model);
      }
      return tool_outcome_rc(outcome);

   } else if (strcmp(action, "shuffle") == 0) {
      /* Explicit on/off sets state; no arg REPORTS current state (deterministic
       * for an LLM — no blind toggle). */
      bool set_on = false, do_set = false;
      if (strcasecmp(base_query, "on") == 0 || strcasecmp(base_query, "true") == 0) {
         set_on = true;
         do_set = true;
      } else if (strcasecmp(base_query, "off") == 0 || strcasecmp(base_query, "false") == 0) {
         do_set = true;
      }

      if (!do_set) {
         pthread_mutex_lock(&uq->queue_mutex);
         bool cur = uq->shuffle;
         pthread_mutex_unlock(&uq->queue_mutex);
         if (result_out) {
            char buf[96];
            snprintf(buf, sizeof(buf),
                     "Shuffle is %s. Use 'shuffle on' or 'shuffle off' to set it.",
                     cur ? "on" : "off");
            *result_out = strdup(buf);
         }
         return SUCCESS;
      }

      pthread_mutex_lock(&uq->queue_mutex);
      uq->shuffle = set_on;
      music_queue_changed(uq, false);
      pthread_mutex_unlock(&uq->queue_mutex);

      music_player_notify_state(player);
      music_queue_announce(uq, player);

      if (result_out) {
         *result_out = result_with_footer(player, set_on ? "Shuffle enabled" : "Shuffle disabled",
                                          for_model);
      }
      return SUCCESS;

   } else if (strcmp(action, "repeat") == 0) {
      /* Explicit none|all|one sets the mode (off == none); no arg REPORTS it. */
      music_repeat_mode_t target = MUSIC_REPEAT_NONE;
      bool do_set = false;
      if (strcasecmp(base_query, "none") == 0 || strcasecmp(base_query, "off") == 0) {
         target = MUSIC_REPEAT_NONE;
         do_set = true;
      } else if (strcasecmp(base_query, "all") == 0) {
         target = MUSIC_REPEAT_ALL;
         do_set = true;
      } else if (strcasecmp(base_query, "one") == 0) {
         target = MUSIC_REPEAT_ONE;
         do_set = true;
      }

      if (!do_set) {
         pthread_mutex_lock(&uq->queue_mutex);
         music_repeat_mode_t cur = uq->repeat_mode;
         pthread_mutex_unlock(&uq->queue_mutex);
         const char *cs = cur == MUSIC_REPEAT_ALL ? "all"
                                                  : (cur == MUSIC_REPEAT_ONE ? "one" : "none");
         if (result_out) {
            char buf[96];
            snprintf(buf, sizeof(buf), "Repeat is %s. Use 'repeat none|all|one' to set it.", cs);
            *result_out = strdup(buf);
         }
         return SUCCESS;
      }

      pthread_mutex_lock(&uq->queue_mutex);
      uq->repeat_mode = target;
      music_queue_changed(uq, false);
      pthread_mutex_unlock(&uq->queue_mutex);

      music_player_notify_state(player);
      music_queue_announce(uq, player);

      const char *ms = target == MUSIC_REPEAT_ALL
                           ? "Repeat all"
                           : (target == MUSIC_REPEAT_ONE ? "Repeat one" : "Repeat off");
      if (result_out) {
         *result_out = result_with_footer(player, ms, for_model);
      }
      return SUCCESS;

   } else if (strcmp(action, "list") == 0) {
      pthread_mutex_lock(&uq->queue_mutex);
      if (uq->queue_length == 0) {
         pthread_mutex_unlock(&uq->queue_mutex);
         if (result_out) {
            *result_out = strdup("No music in queue");
         }
         return SUCCESS;
      }

      /* What the player reports as playing, read with the queue held */
      music_queue_entry_t now_entry;
      int cur_idx = music_player_current(player, &now_entry);
      bool now = now_entry.path[0] != '\0';
      const char *now_title = now_entry.title;

      /* Build queue list — strbuf so a long queue cannot silently truncate
       * mid-row the way the prior fixed sizing did when artist/title strings
       * exceeded the per-row estimate. */
      strbuf_t sb;
      strbuf_init(&sb, 1024 + (size_t)uq->queue_length * 64);
      if (cur_idx >= 0) {
         strbuf_appendf(&sb, "Queue (%d tracks, currently #%d):\n", uq->queue_length, cur_idx + 1);
      } else if (now) {
         strbuf_appendf(&sb, "Queue (%d tracks; playing \"%s\", which isn't in it):\n",
                        uq->queue_length, now_title[0] ? now_title : "Unknown");
      } else {
         strbuf_appendf(&sb, "Queue (%d tracks):\n", uq->queue_length);
      }
      const char *rep_s = uq->repeat_mode == MUSIC_REPEAT_ALL
                              ? "all"
                              : (uq->repeat_mode == MUSIC_REPEAT_ONE ? "one" : "none");
      strbuf_appendf(&sb, "Shuffle: %s | Repeat: %s\n", uq->shuffle ? "on" : "off", rep_s);
      for (int i = 0; i < uq->queue_length; i++) {
         const char *marker = (i == cur_idx) ? "\xE2\x96\xB6 " : "  ";
         if (strbuf_appendf(&sb, "%s%d. %s - %s\n", marker, i + 1,
                            uq->queue[i].artist[0] ? uq->queue[i].artist : "Unknown",
                            uq->queue[i].title[0] ? uq->queue[i].title : "Unknown") < 0)
            break;
      }
      if (result_out) {
         char *out = strbuf_oom(&sb) ? NULL : strbuf_steal(&sb);
         *result_out = out;
         if (!out)
            strbuf_free(&sb);
      } else {
         strbuf_free(&sb);
      }
      pthread_mutex_unlock(&uq->queue_mutex);
      return SUCCESS;
   }

   if (result_out) {
      char buf[128];
      snprintf(buf, sizeof(buf), "Unknown action: %s", action);
      *result_out = strdup(buf);
   }
   return FAILURE;
}
