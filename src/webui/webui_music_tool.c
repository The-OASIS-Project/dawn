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
 * WebUI Music - LLM tool integration
 *
 * The music/volume tool entry points the daemon calls when a music command
 * originates from a WebUI session. Split from webui_music_handlers.c (which
 * keeps the browser-driven WebSocket message handlers) to stay under the file
 * size limit and to keep the LLM-tool surface cohesive.
 *
 * IMPORTANT: webui_music_execute_tool() runs on the LLM worker thread, NOT the
 * LWS service thread. All WebSocket sends here must go through thread-safe
 * paths (webui_music_send_state/send_error use queue_response() internally).
 * Never call send_json_response() or lws_write() directly from here.
 */

#define _GNU_SOURCE /* strcasestr */

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "audio/music_db.h"
#include "audio/music_source.h"
#include "core/path_utils.h"
#include "core/strbuf.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/tool_registry.h"
#include "tools/volume_tool.h"
#include "utils/string_utils.h"
#include "webui/webui_music_internal.h"
#include "webui/webui_music_queue_db.h"
#include "webui/webui_server.h"

/* "Artist - Title" display string size for resolved items. */
#define WEBUI_MUSIC_DISPLAY_MAX (WEBUI_MUSIC_STRING_MAX * 2)

/* Candidate window pulled from the DB for relevance ranking in the resolver. */
#define WEBUI_MUSIC_RESOLVE_CANDIDATES 8

/* =============================================================================
 * LLM-facing helpers: state footer, item resolution, batch apply + report
 * ============================================================================= */

/**
 * @brief Append a compact "[queue: ... | now ... | shuffle | repeat]" footer
 *
 * Gives the LLM the resulting player state after every mutating action so it
 * never has to call list to stay oriented. queue_index is read under
 * queue_mutex (display-only, matches the existing list action's pattern).
 */
static void append_state_footer(strbuf_t *sb, session_music_state_t *state) {
   user_music_queue_t *uq = state->shared_queue;
   char now_title[WEBUI_MUSIC_STRING_MAX] = { 0 };
   char now_artist[WEBUI_MUSIC_STRING_MAX] = { 0 };

   pthread_mutex_lock(&uq->queue_mutex);
   int len = uq->queue_length;
   bool shuffle = uq->shuffle;
   music_repeat_mode_t rep = uq->repeat_mode;
   /* queue_index is a per-session field guarded by state_mutex; snapshot it under
    * that lock (nested inside queue_mutex — correct order) to avoid a torn read. */
   pthread_mutex_lock(&state->state_mutex);
   int idx = state->queue_index;
   pthread_mutex_unlock(&state->state_mutex);
   if (len > 0 && idx >= 0 && idx < len) {
      safe_strncpy(now_title, uq->queue[idx].title, sizeof(now_title));
      safe_strncpy(now_artist, uq->queue[idx].artist, sizeof(now_artist));
   }
   pthread_mutex_unlock(&uq->queue_mutex);

   const char *rep_s = rep == MUSIC_REPEAT_ALL ? "all" : (rep == MUSIC_REPEAT_ONE ? "one" : "none");
   if (len > 0) {
      strbuf_appendf(sb, "\n[queue: %d | now #%d \"%s\" — %s | shuffle %s | repeat %s]", len,
                     idx + 1, now_title[0] ? now_title : "Unknown",
                     now_artist[0] ? now_artist : "Unknown", shuffle ? "on" : "off", rep_s);
   } else {
      strbuf_appendf(sb, "\n[queue: empty | shuffle %s | repeat %s]", shuffle ? "on" : "off",
                     rep_s);
   }
}

/** strdup(msg) with the state footer appended; falls back to plain msg on OOM. */
static char *result_with_footer(session_music_state_t *state, const char *msg) {
   strbuf_t sb;
   strbuf_init(&sb, 256);
   strbuf_appendf(&sb, "%s", msg);
   append_state_footer(&sb, state);
   if (strbuf_oom(&sb)) {
      strbuf_free(&sb);
      return strdup(msg);
   }
   return strbuf_steal(&sb);
}

/**
 * @brief Resolve a free-text track name or library path to a playable path
 *
 * Accepts an absolute library path (exact lookup) or a name (best-match
 * search). For an "Artist - Title" item we search the TITLE portion (spaces are
 * intra-field wildcards, so the combined form usually misses) and prefer a
 * candidate whose artist matches the artist portion — this disambiguates
 * generic titles (e.g. "Toto - Africa" picks Toto's track, not an unrelated
 * top match). Returns SUCCESS and fills path_out (+ optional display).
 */
static int resolve_item_to_path(const char *item,
                                char *path_out,
                                size_t path_len,
                                char *display_out,
                                size_t display_len) {
   if (!item || !item[0]) {
      return FAILURE;
   }

   music_search_result_t r[WEBUI_MUSIC_RESOLVE_CANDIDATES];
   int count = 0;

   /* Exact library path → exact lookup. webui_music_is_path_valid() accepts both
    * local absolute paths and validated plex: paths (the forms 'search' returns),
    * and rejects free-text names — so names fall through to the ranked search
    * below. Doubles as defense in depth (no out-of-library file). */
   if (webui_music_is_path_valid(item)) {
      bool found = false;
      if (music_db_get_by_path(item, &r[0], &found) == SUCCESS && found) {
         safe_strncpy(path_out, r[0].path, path_len);
         if (display_out) {
            /* Format into a full-size buffer (can't truncate → no
             * -Wformat-truncation), then copy-truncate into the caller's. */
            char label[2 * AUDIO_METADATA_STRING_MAX + 4];
            snprintf(label, sizeof(label), "%s - %s", r[0].artist, r[0].title);
            safe_strncpy(display_out, label, display_len);
         }
         return SUCCESS;
      }
      return FAILURE;
   }

   /* Split an optional "Artist - Title"; search the title (or whole item) and
    * rank the candidates by relevance (music_db_search orders alphabetically, so
    * its first row is rarely the right track — e.g. "Africa" → Bo Burnham). */
   const char *dash = strstr(item, " - ");
   char artist[sizeof(r[0].artist)] = { 0 };
   const char *title_query = item;
   if (dash) {
      size_t alen = (size_t)(dash - item);
      if (alen >= sizeof(artist)) {
         alen = sizeof(artist) - 1;
      }
      memcpy(artist, item, alen);
      artist[alen] = '\0';
      title_query = dash + 3;
   }

   if (music_db_search(title_query, r, WEBUI_MUSIC_RESOLVE_CANDIDATES, &count) == SUCCESS &&
       count > 0) {
      int pick = music_db_pick_best_match(r, count, title_query, dash ? artist : NULL);
      safe_strncpy(path_out, r[pick].path, path_len);
      if (display_out) {
         char label[2 * AUDIO_METADATA_STRING_MAX + 4];
         snprintf(label, sizeof(label), "%s - %s", r[pick].artist, r[pick].title);
         safe_strncpy(display_out, label, display_len);
      }
      return SUCCESS;
   }
   return FAILURE;
}

/**
 * @brief Decode the terminal ::items:: slot into a JSON string array
 *
 * Uses the tail extractor (the array is the terminal custom slot, per the
 * ARRAY contract). Returns a json_type_array the caller must json_object_put(),
 * or NULL if absent / not an array.
 */
static struct json_object *decode_items(const char *value) {
   if (!value || !value[0]) {
      return NULL;
   }
   size_t cap = strlen(value) + 1;
   char *json = malloc(cap);
   if (!json) {
      return NULL;
   }
   bool got = tool_param_extract_custom_tail(value, "items", json, cap);
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
 * Resolves each entry, calls webui_music_queue_apply_paths(), and builds a
 * grouped "Added / Already in queue / No match" report naming the items so the
 * LLM can self-correct. Caller adds the state footer and handles playback start.
 *
 * @param replace        true → replace the queue (play); false → append (enqueue)
 * @param added_count_out optional: number of tracks actually added
 * @return allocated report string (caller frees), or NULL on OOM
 */
static char *apply_items_and_report(session_music_state_t *state,
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
   if (n > WEBUI_MUSIC_MAX_QUEUE) {
      dropped_over_cap = n - WEBUI_MUSIC_MAX_QUEUE;
      n = WEBUI_MUSIC_MAX_QUEUE;
   }

   char(*paths)[WEBUI_MUSIC_PATH_MAX] = calloc((size_t)n, sizeof(*paths));
   char(*disp)[WEBUI_MUSIC_DISPLAY_MAX] = calloc((size_t)n, sizeof(*disp));
   const char **pptr = calloc((size_t)n, sizeof(*pptr));
   queue_apply_outcome_t *oc = calloc((size_t)n, sizeof(*oc));
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
      if (resolve_item_to_path(item, paths[k], sizeof(paths[k]), disp[k], sizeof(disp[k])) ==
          SUCCESS) {
         pptr[k] = paths[k];
         k++;
      } else if (item && item[0]) {
         strbuf_appendf(&nomatch, "%s%s", nomatch.len ? ", " : "", item);
      }
   }

   webui_music_queue_apply_paths(state, pptr, k, replace, oc);

   strbuf_t added, dup;
   strbuf_init(&added, 256);
   strbuf_init(&dup, 128);
   int added_count = 0;
   for (int j = 0; j < k; j++) {
      if (oc[j] == QUEUE_APPLY_ADDED) {
         strbuf_appendf(&added, "%s%s", added.len ? ", " : "", disp[j]);
         added_count++;
      } else if (oc[j] == QUEUE_APPLY_DUPLICATE) {
         strbuf_appendf(&dup, "%s%s", dup.len ? ", " : "", disp[j]);
      } else if (oc[j] == QUEUE_APPLY_FULL) {
         strbuf_appendf(&nomatch, "%s%s (queue full)", nomatch.len ? ", " : "", disp[j]);
      }
   }

   strbuf_t out;
   strbuf_init(&out, 512);
   strbuf_appendf(&out, "Added %d track%s.", added_count, added_count == 1 ? "" : "s");
   if (added.len) {
      strbuf_appendf(&out, " Matched: %s.", added.buf);
   }
   if (dup.len) {
      strbuf_appendf(&out, " Already in queue: %s.", dup.buf);
   }
   if (nomatch.len) {
      strbuf_appendf(&out, " No match: %s.", nomatch.buf);
   }
   if (dropped_over_cap > 0) {
      strbuf_appendf(&out, " %d item%s beyond the %d-track queue limit were skipped.",
                     dropped_over_cap, dropped_over_cap == 1 ? "" : "s", WEBUI_MUSIC_MAX_QUEUE);
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
   strbuf_free(&dup);
   free(paths);
   free(disp);
   free(pptr);
   free(oc);
   return result;
}


int webui_music_execute_tool(ws_connection_t *conn,
                             const char *action,
                             const char *query,
                             char **result_out) {
   if (!conn || !action) {
      return FAILURE;
   }

   /* Ensure music session is initialized */
   session_music_state_t *state = (session_music_state_t *)conn->music_state;
   if (!state) {
      if (webui_music_session_init(conn) != SUCCESS) {
         if (result_out) {
            *result_out = strdup("Failed to initialize music session");
         }
         return FAILURE;
      }
      state = (session_music_state_t *)conn->music_state;
   }

   OLOG_INFO("WebUI music tool: action='%s' query='%s'", action, query ? query : "(none)");

   /* The bridge passes the packed value string; the real search query is the
    * base segment, and list params arrive in the terminal ::items:: slot. */
   char base_query[WEBUI_MUSIC_PATH_MAX] = { 0 };
   tool_param_extract_base(query ? query : "", base_query, sizeof(base_query));

   if (strcmp(action, "play") == 0) {
      /* items[] → REPLACE the queue with an explicit, resolved track list */
      struct json_object *items = decode_items(query);
      if (items) {
         webui_music_stop_streaming(state);
         pthread_mutex_lock(&state->state_mutex);
         if (state->decoder) {
            audio_decoder_close(state->decoder);
            state->decoder = NULL;
         }
         state->queue_index = 0;
         pthread_mutex_unlock(&state->state_mutex);

         char *report = apply_items_and_report(state, items, true, NULL);
         json_object_put(items);

         user_music_queue_t *uq = state->shared_queue;
         char first_path[WEBUI_MUSIC_PATH_MAX] = { 0 };
         pthread_mutex_lock(&uq->queue_mutex);
         if (uq->queue_length > 0) {
            snprintf(first_path, sizeof(first_path), "%s", uq->queue[0].path);
         }
         pthread_mutex_unlock(&uq->queue_mutex);
         if (first_path[0]) {
            webui_music_start_playback(state, first_path);
         }
         webui_music_send_state(conn, state);
         webui_music_broadcast_queue_state(uq, conn);

         if (result_out) {
            strbuf_t sb;
            strbuf_init(&sb, 512);
            strbuf_appendf(&sb, "Replaced queue. %s Use 'enqueue' to add without clearing.",
                           report ? report : "");
            append_state_footer(&sb, state);
            if (strbuf_oom(&sb)) {
               strbuf_free(&sb);
               *result_out = strdup("Replaced queue");
            } else {
               *result_out = strbuf_steal(&sb);
            }
         }
         free(report);
         return SUCCESS;
      }

      if (base_query[0] == '\0') {
         /* Resume if paused */
         if (state->paused) {
            pthread_mutex_lock(&state->state_mutex);
            state->paused = false;
            pthread_mutex_unlock(&state->state_mutex);
            webui_music_send_state(conn, state);
            if (result_out) {
               *result_out = result_with_footer(state, "Resumed playback");
            }
            return SUCCESS;
         }
         if (result_out) {
            *result_out = strdup(
                "Please specify what to play, or pass items:[...] for a track list");
         }
         return FAILURE;
      }

      /* Search unified database and play (REPLACE) */
      webui_music_stop_streaming(state);
      pthread_mutex_lock(&state->state_mutex);
      if (state->decoder) {
         audio_decoder_close(state->decoder);
         state->decoder = NULL;
      }
      state->queue_index = 0;
      pthread_mutex_unlock(&state->state_mutex);

      if (!music_db_is_initialized()) {
         if (result_out)
            *result_out = strdup("Music database not available");
         return FAILURE;
      }
      music_search_result_t *results = malloc(WEBUI_MUSIC_MAX_QUEUE *
                                              sizeof(music_search_result_t));
      if (!results) {
         if (result_out)
            *result_out = strdup("Memory allocation failed");
         return FAILURE;
      }
      int count = 0;
      if (music_db_search(base_query, results, WEBUI_MUSIC_MAX_QUEUE, &count) != SUCCESS ||
          count <= 0) {
         free(results);
         if (result_out) {
            char buf[WEBUI_MUSIC_PATH_MAX + 64];
            snprintf(buf, sizeof(buf), "No music found matching '%s'", base_query);
            *result_out = strdup(buf);
         }
         return FAILURE;
      }

      user_music_queue_t *uq = state->shared_queue;
      char first_title[WEBUI_MUSIC_STRING_MAX] = { 0 };
      char first_path[WEBUI_MUSIC_PATH_MAX] = { 0 };
      int total_queued = 0;

      pthread_mutex_lock(&uq->queue_mutex);
      uq->queue_length = 0;
      for (int i = 0; i < count && uq->queue_length < WEBUI_MUSIC_MAX_QUEUE; i++) {
         music_queue_entry_t *entry = &uq->queue[uq->queue_length];
         safe_strncpy(entry->path, results[i].path, sizeof(entry->path));
         safe_strncpy(entry->title, results[i].title, sizeof(entry->title));
         safe_strncpy(entry->artist, results[i].artist, sizeof(entry->artist));
         safe_strncpy(entry->album, results[i].album, sizeof(entry->album));
         entry->duration_sec = results[i].duration_sec;
         uq->queue_length++;
      }
      uq->generation++;
      music_queue_db_save(uq->user_id, uq);
      total_queued = uq->queue_length;
      snprintf(first_path, sizeof(first_path), "%s", uq->queue[0].path);
      snprintf(first_title, sizeof(first_title), "%s", uq->queue[0].title);
      pthread_mutex_unlock(&uq->queue_mutex);
      free(results);

      /* Start playback of first track */
      if (webui_music_start_playback(state, first_path) != 0) {
         if (result_out) {
            *result_out = strdup("Failed to start playback");
         }
         return FAILURE;
      }

      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(uq, conn);

      if (result_out) {
         char buf[WEBUI_MUSIC_PATH_MAX + 256];
         snprintf(buf, sizeof(buf),
                  "Replaced queue with %d track%s matching '%s'; now playing #1: %s. "
                  "(Use 'enqueue' to add without clearing.)",
                  total_queued, total_queued == 1 ? "" : "s", base_query,
                  first_title[0] ? first_title : "Unknown");
         *result_out = result_with_footer(state, buf);
      }
      return SUCCESS;

   } else if (strcmp(action, "enqueue") == 0) {
      /* APPEND to the queue — items[] (preferred) or a single query (top match). */
      struct json_object *items = decode_items(query);
      if (!items && base_query[0] == '\0') {
         if (result_out)
            *result_out = strdup("enqueue needs items:[...] or a query");
         return FAILURE;
      }

      char *report = NULL;
      if (items) {
         report = apply_items_and_report(state, items, false, NULL);
         json_object_put(items);
      } else {
         char path[WEBUI_MUSIC_PATH_MAX] = { 0 };
         char disp[WEBUI_MUSIC_DISPLAY_MAX] = { 0 };
         if (resolve_item_to_path(base_query, path, sizeof(path), disp, sizeof(disp)) == SUCCESS) {
            const char *p = path;
            queue_apply_outcome_t oc = QUEUE_APPLY_NOT_FOUND;
            webui_music_queue_apply_paths(state, &p, 1, false, &oc);
            char buf[WEBUI_MUSIC_DISPLAY_MAX + 64];
            if (oc == QUEUE_APPLY_ADDED)
               snprintf(buf, sizeof(buf), "Added %s.", disp);
            else if (oc == QUEUE_APPLY_DUPLICATE)
               snprintf(buf, sizeof(buf), "Already in queue: %s.", disp);
            else
               snprintf(buf, sizeof(buf), "Could not add %s.", disp);
            report = strdup(buf);
         } else {
            char buf[WEBUI_MUSIC_PATH_MAX + 64];
            snprintf(buf, sizeof(buf), "No match: %s.", base_query);
            report = strdup(buf);
         }
      }

      /* enqueue only adds — it never starts playback (the WebUI has its own
       * play control). The user presses play, or calls play/next explicitly. */
      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(state->shared_queue, conn);

      if (result_out) {
         *result_out = result_with_footer(state, report ? report : "Nothing added.");
      }
      free(report);
      return SUCCESS;

   } else if (strcmp(action, "clear") == 0) {
      int removed = 0;
      webui_music_queue_clear(state, &removed);
      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(state->shared_queue, conn);
      if (result_out) {
         char buf[128];
         snprintf(buf, sizeof(buf), "Cleared queue (was %d track%s).", removed,
                  removed == 1 ? "" : "s");
         *result_out = result_with_footer(state, buf);
      }
      return SUCCESS;

   } else if (strcmp(action, "remove") == 0) {
      /* 1-based index, or a substring match against queue titles. */
      if (base_query[0] == '\0') {
         if (result_out)
            *result_out = strdup("remove needs a track number (1-based) or a title to match");
         return FAILURE;
      }

      user_music_queue_t *uq = state->shared_queue;
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
               char buf[WEBUI_MUSIC_PATH_MAX + 64];
               snprintf(buf, sizeof(buf), "No queued track matching '%s'", base_query);
               *result_out = strdup(buf);
            }
            return FAILURE;
         }
      }

      if (webui_music_queue_remove_index(state, index0) != SUCCESS) {
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
      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(uq, conn);
      if (result_out) {
         char buf[64];
         snprintf(buf, sizeof(buf), "Removed track %d.", index0 + 1);
         *result_out = result_with_footer(state, buf);
      }
      return SUCCESS;

   } else if (strcmp(action, "stop") == 0) {
      webui_music_stop_streaming(state);
      pthread_mutex_lock(&state->state_mutex);
      state->playing = false;
      state->paused = false;
      if (state->decoder) {
         audio_decoder_close(state->decoder);
         state->decoder = NULL;
      }
      state->position_frames = 0;
      pthread_mutex_unlock(&state->state_mutex);
      webui_music_send_state(conn, state);

      if (result_out) {
         *result_out = result_with_footer(state, "Music playback stopped");
      }
      return SUCCESS;

   } else if (strcmp(action, "pause") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      state->paused = true;
      pthread_mutex_unlock(&state->state_mutex);
      webui_music_send_state(conn, state);

      if (result_out) {
         *result_out = result_with_footer(state, "Music paused");
      }
      return SUCCESS;

   } else if (strcmp(action, "resume") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      state->paused = false;
      pthread_mutex_unlock(&state->state_mutex);
      webui_music_send_state(conn, state);

      if (result_out) {
         *result_out = result_with_footer(state, "Music resumed");
      }
      return SUCCESS;

   } else if (strcmp(action, "next") == 0) {
      user_music_queue_t *uq = state->shared_queue;
      bool can_advance = false;
      char next_path[WEBUI_MUSIC_PATH_MAX] = { 0 };
      char next_title[WEBUI_MUSIC_STRING_MAX] = { 0 };

      /* Hold queue_mutex across the full computation (hierarchy: queue → state) */
      pthread_mutex_lock(&uq->queue_mutex);
      int q_len = uq->queue_length;
      bool q_shuffle = uq->shuffle;
      music_repeat_mode_t q_repeat = uq->repeat_mode;

      pthread_mutex_lock(&state->state_mutex);
      if (q_len > 0) {
         if (q_shuffle && q_len > 1) {
            state->queue_index = webui_music_pick_random_index(state->queue_index, q_len,
                                                               &state->shuffle_seed);
            can_advance = true;
         } else if (state->queue_index < q_len - 1) {
            state->queue_index++;
            can_advance = true;
         } else if (q_repeat == MUSIC_REPEAT_ALL) {
            state->queue_index = 0;
            can_advance = true;
         }
      }
      if (can_advance && state->queue_index < q_len) {
         snprintf(next_path, sizeof(next_path), "%s", uq->queue[state->queue_index].path);
         snprintf(next_title, sizeof(next_title), "%s", uq->queue[state->queue_index].title);
      }
      pthread_mutex_unlock(&state->state_mutex);
      pthread_mutex_unlock(&uq->queue_mutex);

      if (can_advance && next_path[0]) {
         webui_music_start_playback(state, next_path);
         webui_music_send_state(conn, state);

         if (result_out) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Playing next: %s", next_title[0] ? next_title : "Unknown");
            *result_out = result_with_footer(state, buf);
         }
      } else {
         if (result_out) {
            *result_out = strdup("Already at end of queue");
         }
      }
      return SUCCESS;

   } else if (strcmp(action, "previous") == 0) {
      user_music_queue_t *uq = state->shared_queue;
      bool can_go_back = false;
      char prev_path[WEBUI_MUSIC_PATH_MAX] = { 0 };
      char prev_title[WEBUI_MUSIC_STRING_MAX] = { 0 };

      /* Hold queue_mutex across the full computation (hierarchy: queue → state) */
      pthread_mutex_lock(&uq->queue_mutex);
      int q_len = uq->queue_length;
      bool q_shuffle = uq->shuffle;
      music_repeat_mode_t q_repeat = uq->repeat_mode;

      pthread_mutex_lock(&state->state_mutex);
      if (q_len > 0) {
         if (q_shuffle && q_len > 1) {
            state->queue_index = webui_music_pick_random_index(state->queue_index, q_len,
                                                               &state->shuffle_seed);
            can_go_back = true;
         } else if (state->queue_index > 0) {
            state->queue_index--;
            can_go_back = true;
         } else if (q_repeat == MUSIC_REPEAT_ALL) {
            state->queue_index = q_len - 1;
            can_go_back = true;
         }
      }
      if (can_go_back && state->queue_index < q_len) {
         snprintf(prev_path, sizeof(prev_path), "%s", uq->queue[state->queue_index].path);
         snprintf(prev_title, sizeof(prev_title), "%s", uq->queue[state->queue_index].title);
      }
      pthread_mutex_unlock(&state->state_mutex);
      pthread_mutex_unlock(&uq->queue_mutex);

      if (can_go_back && prev_path[0]) {
         webui_music_start_playback(state, prev_path);
         webui_music_send_state(conn, state);

         if (result_out) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Playing previous: %s",
                     prev_title[0] ? prev_title : "Unknown");
            *result_out = result_with_footer(state, buf);
         }
      } else {
         if (result_out) {
            *result_out = strdup("Already at start of queue");
         }
      }
      return SUCCESS;

   } else if (strcmp(action, "shuffle") == 0) {
      user_music_queue_t *uq = state->shared_queue;

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
      uq->generation++;
      music_queue_db_save_state(uq->user_id, uq);
      pthread_mutex_unlock(&uq->queue_mutex);

      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(uq, conn);

      if (result_out) {
         *result_out = result_with_footer(state, set_on ? "Shuffle enabled" : "Shuffle disabled");
      }
      return SUCCESS;

   } else if (strcmp(action, "repeat") == 0) {
      user_music_queue_t *uq = state->shared_queue;

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
      uq->generation++;
      music_queue_db_save_state(uq->user_id, uq);
      pthread_mutex_unlock(&uq->queue_mutex);

      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(uq, conn);

      const char *ms = target == MUSIC_REPEAT_ALL
                           ? "Repeat all"
                           : (target == MUSIC_REPEAT_ONE ? "Repeat one" : "Repeat off");
      if (result_out) {
         *result_out = result_with_footer(state, ms);
      }
      return SUCCESS;

   } else if (strcmp(action, "list") == 0) {
      user_music_queue_t *uq = state->shared_queue;
      pthread_mutex_lock(&uq->queue_mutex);
      if (uq->queue_length == 0) {
         pthread_mutex_unlock(&uq->queue_mutex);
         if (result_out) {
            *result_out = strdup("No music in queue");
         }
         return SUCCESS;
      }

      /* Snapshot the per-session current index under state_mutex (nested in
       * queue_mutex — correct order) rather than reading it racily. */
      pthread_mutex_lock(&state->state_mutex);
      int cur_idx = state->queue_index;
      pthread_mutex_unlock(&state->state_mutex);

      /* Build queue list — strbuf so a long queue cannot silently truncate
       * mid-row the way the prior fixed sizing did when artist/title strings
       * exceeded the per-row estimate. */
      strbuf_t sb;
      strbuf_init(&sb, 1024 + (size_t)uq->queue_length * 64);
      strbuf_appendf(&sb, "Queue (%d tracks, currently #%d):\n", uq->queue_length, cur_idx + 1);
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

   } else if (strcmp(action, "search") == 0 || strcmp(action, "library") == 0) {
      /* Batch search: items[] of queries → grouped results WITH paths so the LLM
       * can enqueue exact tracks. Single-query search/library fall through to the
       * local handler (they don't affect streaming state). */
      struct json_object *items = (strcmp(action, "search") == 0) ? decode_items(query) : NULL;
      if (items) {
         int n = (int)json_object_array_length(items);
         if (n > WEBUI_MUSIC_MAX_QUEUE) {
            n = WEBUI_MUSIC_MAX_QUEUE; /* bound per-call DB work (one LIKE scan each) */
         }
         strbuf_t sb;
         strbuf_init(&sb, 1024);
         for (int i = 0; i < n; i++) {
            struct json_object *e = json_object_array_get_idx(items, i);
            const char *q = e ? json_object_get_string(e) : NULL;
            if (!q || !q[0]) {
               continue;
            }
            strbuf_appendf(&sb, "%s:\n", q);
            music_search_result_t r[5];
            int count = 0;
            if (music_db_search(q, r, 5, &count) == SUCCESS && count > 0) {
               for (int j = 0; j < count; j++) {
                  strbuf_appendf(&sb, "  %s - %s  [%s]\n", r[j].artist, r[j].title, r[j].path);
               }
            } else {
               strbuf_appendf(&sb, "  No match: %s\n", q);
            }
         }
         json_object_put(items);
         if (result_out) {
            char *out = strbuf_oom(&sb) ? NULL : strbuf_steal(&sb);
            *result_out = out;
            if (!out) {
               strbuf_free(&sb);
            }
         } else {
            strbuf_free(&sb);
         }
         return SUCCESS;
      }

      /* Single-query search/library — let music_tool's local handler answer. */
      if (result_out) {
         *result_out = NULL; /* Signal to fall through to regular handler */
      }
      return MUSIC_NOT_HANDLED;
   }

   if (result_out) {
      char buf[128];
      snprintf(buf, sizeof(buf), "Unknown action: %s", action);
      *result_out = strdup(buf);
   }
   return FAILURE;
}

/* =============================================================================
 * Volume Tool Integration (called from volume_tool.c via session routing)
 * ============================================================================= */

char *webui_volume_execute_tool(ws_connection_t *conn,
                                const char *action,
                                const char *value,
                                int *should_respond) {
   *should_respond = 1;

   if (strcmp(action, "get") == 0) {
      char *result = malloc(64);
      if (result)
         snprintf(result, 64, "Volume is at %.0f%%", conn->volume * 100.0f);
      return result;
   }

   /* Action: set */
   if (!value || !*value) {
      return strdup("Error: 'level' parameter is required for 'set' action.");
   }

   float vol = parse_volume_level(value);
   if (vol < 0.0f) {
      char *result = malloc(128);
      if (result)
         snprintf(result, 128, "Invalid volume level '%s'. Use a number 0-100.", value);
      return result;
   }

   conn->volume = vol;
   OLOG_INFO("WebUI volume tool: set to %.0f%% for session", vol * 100.0f);

   /* Send state update to client so slider syncs */
   session_music_state_t *state = (session_music_state_t *)conn->music_state;
   if (state) {
      webui_music_send_state(conn, state);
   }

   char *result = malloc(64);
   if (result)
      snprintf(result, 64, "Volume set to %.0f%%", vol * 100.0f);
   return result;
}
