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
 * Admin-socket handler for LLM request capture (ADMIN_MSG_LLM_CAPTURE): arms
 * or stops writing one user's next requests to files, for the quality suite.
 */

#define ADMIN_SOCKET_INTERNAL_ALLOWED

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/admin_socket.h"
#include "auth/admin_socket_internal.h"
#include "auth/auth_db.h"
#include "dawn_error.h"
#include "llm/llm_request_capture.h"
#include "logging.h"

/* The next NUL-terminated field of @p len bytes at @p *pos into @p out. */
static int next_field(const char *buf, size_t len, size_t *pos, char *out, size_t out_sz) {
   size_t end = *pos;
   while (end < len && buf[end] != '\0') {
      end++;
   }
   const size_t n = end - *pos;
   if (n == 0 || n >= out_sz) {
      return FAILURE;
   }
   memcpy(out, buf + *pos, n);
   out[n] = '\0';
   *pos = end < len ? end + 1 : end;
   return SUCCESS;
}

int handle_llm_capture_cmd(int client_fd, const char *payload, uint16_t payload_len) {
   size_t auth_size = 0;
   if (verify_admin_auth(payload, payload_len, &auth_size) != 0) {
      return send_response(client_fd, ADMIN_RESP_UNAUTHORIZED);
   }
   const char *body = payload + auth_size;
   const size_t len = (size_t)payload_len - auth_size;
   char user[AUTH_USERNAME_MAX];
   char dir[LLM_CAPTURE_DIR_MAX + 1];
   char count_text[8];
   size_t pos = 0;
   if (next_field(body, len, &pos, user, sizeof(user)) != SUCCESS ||
       next_field(body, len, &pos, dir, sizeof(dir)) != SUCCESS ||
       next_field(body, len, &pos, count_text, sizeof(count_text)) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "Usage: llm capture --user <name> --out <dir> [--requests N]");
   }
   char *end = NULL;
   const long count = strtol(count_text, &end, 10);
   if (!end || *end != '\0' || count < 0 || count > LLM_CAPTURE_COUNT_MAX) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "count must be 0-50");
   }
   if (count == 0) {
      llm_request_capture_disarm();
      auth_db_log_event("LLM_CAPTURE_STOP", NULL, NULL, NULL);
      return send_text_response(client_fd, ADMIN_RESP_SUCCESS, "Capture stopped");
   }
   auth_user_t u;
   if (auth_db_get_user(user, &u) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "User not found");
   }
   char err[96] = "";
   if (llm_request_capture_arm(u.id, dir, (int)count, err, sizeof(err)) != SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, err);
   }
   auth_db_log_event("LLM_CAPTURE", user, NULL, dir);
   char msg[LLM_CAPTURE_DIR_MAX + AUTH_USERNAME_MAX + 64];
   snprintf(msg, sizeof(msg), "Capturing the next %ld request(s) for '%s' to %s (stops in %d min)",
            count, user, dir, LLM_CAPTURE_TTL_SEC / 60);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}
