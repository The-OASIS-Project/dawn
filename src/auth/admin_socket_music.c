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
 * Music-database admin handlers (stats/search/list/rescan) for the dawn-admin
 * CLI.  Extracted from admin_socket.c; dispatched from handle_client() against
 * the ADMIN_MSG_MUSIC_* opcodes.
 */

#define ADMIN_SOCKET_INTERNAL_ALLOWED

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "audio/music_db.h"
#include "audio/music_scanner.h"
#include "auth/admin_socket_internal.h"
#include "dawn_error.h"

int admin_handle_music_stats(int client_fd) {
   if (!music_db_is_initialized()) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR,
                                "Music database not initialized");
   }

   music_db_stats_t stats;
   if (music_db_get_stats(&stats) != 0) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "Failed to get music stats");
   }

   bool scanner_running = music_scanner_is_running();
   bool initial_complete = music_scanner_initial_scan_complete();

   char response[ADMIN_MSG_CONTENT_MAX];
   snprintf(response, sizeof(response),
            "Music Database Statistics\n"
            "-------------------------\n"
            "Tracks:  %d\n"
            "Artists: %d\n"
            "Albums:  %d\n"
            "Scanner: %s\n"
            "Status:  %s",
            stats.track_count, stats.artist_count, stats.album_count,
            scanner_running ? "running" : "stopped", initial_complete ? "ready" : "indexing");

   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, response);
}

int admin_handle_music_search(int client_fd, const char *payload, uint16_t len) {
   if (len == 0 || len > 200) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid search query");
   }

   if (!music_db_is_initialized()) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR,
                                "Music database not initialized");
   }

   /* Allocate results on heap */
   music_search_result_t *results = malloc(50 * sizeof(music_search_result_t));
   if (!results) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "Memory allocation failed");
   }

   int count = 0;
   /* Ranked, path-clean query — same relevance as the assistant/browser surfaces. */
   music_query_t mq = { .text = payload };
   if (music_db_query(&mq, results, 50, &count) != SUCCESS) {
      free(results);
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "Search failed");
   }

   if (count == 0) {
      free(results);
      return send_text_response(client_fd, ADMIN_RESP_SUCCESS, "No results found");
   }

   /* Build response */
   char response[ADMIN_MSG_CONTENT_MAX];
   int offset = snprintf(response, sizeof(response), "Found %d result(s):\n", count);

   for (int i = 0; i < count && offset < (int)sizeof(response) - 100; i++) {
      offset += snprintf(response + offset, sizeof(response) - offset, "%d. %s\n", i + 1,
                         results[i].display_name);
   }

   free(results);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, response);
}

int admin_handle_music_list(int client_fd, const char *payload, uint16_t len) {
   if (!music_db_is_initialized()) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR,
                                "Music database not initialized");
   }

   /* Parse limit from payload (0 or empty = all tracks) */
   int limit = 0;
   if (len > 0) {
      limit = atoi(payload);
   }
   /* 0 means show all, cap at reasonable max for response size */
   if (limit <= 0) {
      limit = 1000;
   } else if (limit > 1000) {
      limit = 1000;
   }

   /* List all tracks (no pattern filtering) */
   music_search_result_t *results = malloc(limit * sizeof(music_search_result_t));
   if (!results) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "Memory allocation failed");
   }

   int count = 0;
   if (music_db_list(results, limit, &count) != SUCCESS) {
      free(results);
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "List failed");
   }

   if (count == 0) {
      free(results);
      return send_text_response(client_fd, ADMIN_RESP_SUCCESS, "No tracks in database");
   }

   /* Build response */
   char response[ADMIN_MSG_CONTENT_MAX];
   int offset = snprintf(response, sizeof(response), "Showing %d track(s):\n", count);

   for (int i = 0; i < count && offset < (int)sizeof(response) - 100; i++) {
      offset += snprintf(response + offset, sizeof(response) - offset, "%d. %s\n", i + 1,
                         results[i].display_name);
   }

   free(results);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, response);
}

int admin_handle_music_rescan(int client_fd) {
   if (!music_scanner_is_running()) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "Music scanner not running");
   }

   music_scanner_trigger_rescan();
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, "Rescan triggered");
}
