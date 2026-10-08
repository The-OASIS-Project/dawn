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
 * libFuzzer harness for reading a CalDAV server's responses (caldav_client.c):
 * any bytes as a calendar-query REPORT (multistatus XML with iCalendar inside,
 * read by libxml2 and libical) or as one sync-collection page.  The server is
 * whoever the user's account points at.  The first byte picks the parser.
 * Built by tests/fuzz/run_fuzzers.sh.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tools/caldav_client.h"

#define BASE_URL "https://cal.example.com/dav/calendars/user/home/"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
   if (size < 1)
      return 0;
   const bool sync_page = (data[0] & 1) != 0;
   const char *xml = (const char *)data + 1;
   size--;

   if (sync_page) {
      caldav_sync_change_t *changes = NULL;
      int count = 0;
      int cap = 0;
      char token[1024];
      bool more = false;
      if (caldav_parse_sync_page(xml, (int)size, BASE_URL, &changes, &count, &cap, token,
                                 sizeof(token), &more) == CALDAV_OK) {
         if (count < 0 || count > cap || memchr(token, '\0', sizeof(token)) == NULL)
            abort();
         for (int i = 0; i < count; i++) {
            if (memchr(changes[i].href, '\0', sizeof(changes[i].href)) == NULL)
               abort();
         }
      }
      free(changes);
   } else {
      caldav_event_list_t list;
      if (caldav_parse_events(xml, size, BASE_URL, &list) == CALDAV_OK && list.count < 0)
         abort();
      caldav_event_list_free(&list);
   }
   return 0;
}
