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
 * dawn-admin "llm" commands: "llm capture" arms the daemon to write one user's
 * next LLM requests to a directory, for the LLM quality suite.
 */

#include "cmd_llm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "password_prompt.h"
#include "socket_client.h"

/* Requests captured when --requests isn't given. */
#define CAPTURE_DEFAULT_REQUESTS 10

static void usage(const char *prog) {
   fprintf(stderr,
           "Usage: %s llm capture --user <name> --out <dir> [--requests N]\n"
           "       %s llm capture --stop\n",
           prog, prog);
}

int cmd_llm(int argc, char *argv[]) {
   if (argc < 3 || strcmp(argv[2], "capture") != 0) {
      usage(argv[0]);
      return 1;
   }
   const char *user = NULL;
   const char *dir = NULL;
   int count = CAPTURE_DEFAULT_REQUESTS;
   int stop = 0;
   for (int i = 3; i < argc; i++) {
      if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
         user = argv[++i];
      } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
         dir = argv[++i];
      } else if (strcmp(argv[i], "--requests") == 0 && i + 1 < argc) {
         count = atoi(argv[++i]);
      } else if (strcmp(argv[i], "--stop") == 0) {
         stop = 1;
      } else {
         usage(argv[0]);
         return 1;
      }
   }
   if (stop) {
      user = "-";
      dir = "-";
      count = 0;
   } else if (!user || !dir || count < 1) {
      usage(argv[0]);
      return 1;
   }

   char admin_user[64] = { 0 };
   char admin_pass[PASSWORD_MAX_LENGTH] = { 0 };
   printf("Admin authentication required for LLM request capture\n\n");
   if (prompt_input("Admin username: ", admin_user, sizeof(admin_user)) != 0 ||
       prompt_password("Admin password: ", admin_pass, sizeof(admin_pass)) != 0) {
      fprintf(stderr, "Error: Failed to read admin credentials\n");
      secure_clear(admin_pass, sizeof(admin_pass));
      return 1;
   }
   int fd = admin_client_connect();
   if (fd < 0) {
      secure_clear(admin_pass, sizeof(admin_pass));
      return 1;
   }
   char response[512] = "";
   admin_resp_code_t resp = admin_client_llm_capture(fd, admin_user, admin_pass, user, dir, count,
                                                     response, sizeof(response));
   secure_clear(admin_pass, sizeof(admin_pass));
   admin_client_disconnect(fd);
   if (resp != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
      return 1;
   }
   printf("%s\n", response);
   return 0;
}
