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
 * dawn-admin "memory superseded" lists a user's merged or replaced facts (a
 * fact a newer value or a merge hid from recall); "memory restore" brings one
 * back.
 */

#include "cmd_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "socket_client.h"

static void usage(const char *prog) {
   fprintf(stderr,
           "Usage: %s memory superseded --user <name> [--grep <text>] [--limit N]\n"
           "       %s memory restore --user <name> --fact-id <id>\n",
           prog, prog);
}

int cmd_memory_facts(int argc, char *argv[]) {
   const char *sub = argc > 2 ? argv[2] : "";
   const int superseded = strcmp(sub, "superseded") == 0;
   if (!superseded && strcmp(sub, "restore") != 0) {
      usage(argv[0]);
      return 1;
   }
   const char *user = NULL;
   const char *grep = NULL;
   long long fact_id = 0;
   int limit = 0;
   for (int i = 3; i < argc; i++) {
      if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
         user = argv[++i];
      } else if (superseded && strcmp(argv[i], "--grep") == 0 && i + 1 < argc) {
         grep = argv[++i];
      } else if (superseded && strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
         limit = atoi(argv[++i]);
      } else if (!superseded && strcmp(argv[i], "--fact-id") == 0 && i + 1 < argc) {
         fact_id = strtoll(argv[++i], NULL, 10);
      } else {
         usage(argv[0]);
         return 1;
      }
   }
   if (!user || (!superseded && fact_id <= 0)) {
      usage(argv[0]);
      return 1;
   }
   if (grep && strlen(grep) > ADMIN_MEM_ENTITY_REASON_MAX) {
      fprintf(stderr, "Error: --grep text is limited to %d characters\n",
              ADMIN_MEM_ENTITY_REASON_MAX);
      return 1;
   }
   int fd = admin_client_connect();
   if (fd < 0) {
      return 1;
   }
   char response[ADMIN_MSG_CONTENT_MAX + 1] = "";
   admin_resp_code_t resp = superseded
                                ? admin_client_memory_fact_superseded(fd, user, grep, limit,
                                                                      response, sizeof(response))
                                : admin_client_memory_fact_restore(fd, user, fact_id, response,
                                                                   sizeof(response));
   admin_client_disconnect(fd);
   if (resp != ADMIN_RESP_SUCCESS) {
      fprintf(stderr, "Error: %s\n", response[0] ? response : admin_resp_strerror(resp));
      return 1;
   }
   printf("%s\n", response);
   return 0;
}
