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
 * Clock times in a named zone through libical, without the process-wide TZ.
 */

#define _GNU_SOURCE /* timegm, tm_gmtoff */

#include "core/tz.h"

#include <libical/ical.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/iso8601.h"
#include "logging.h"

/* tzdata's own list of zones and their old names ("L <current> <old>") */
#define TZ_TZDATA_ZI "/usr/share/zoneinfo/tzdata.zi"
#define TZ_ZONEINFO_DIR "/usr/share/zoneinfo/"

/* Most old names read from tzdata.zi; it has about 250. */
#define TZ_MAX_LINKS 1024

/* Largest zone file compared when /etc/localtime is a copy (they're ~3 KB). */
#define TZ_MAX_ZONE_FILE 65536

#define TZ_NAME_LEN 64
#define TZ_DAY 86400

/* ---- Lookup -------------------------------------------------------------- */

/* libical's zones and their names, copied once: libical replaces (and frees)
 * a zone's name when it first loads the zone, so its strings can't be read
 * while another thread converts.  The zones themselves don't move (libical
 * keeps its list in chunks). */
static struct {
   pthread_once_t once;
   char (*names)[TZ_NAME_LEN];
   tz_t **zones;
   size_t count;
} s_zones = { .once = PTHREAD_ONCE_INIT };

static void load_zones(void) {
   icalarray *zones = icaltimezone_get_builtin_timezones();
   const size_t n = zones ? zones->num_elements : 0;
   s_zones.names = calloc(n ? n : 1, sizeof(*s_zones.names));
   s_zones.zones = calloc(n ? n : 1, sizeof(*s_zones.zones));
   if (!s_zones.names || !s_zones.zones)
      return;
   for (size_t i = 0; i < n; i++) {
      tz_t *z = icalarray_element_at(zones, i);
      const char *name = icaltimezone_get_location(z);
      if (name) {
         snprintf(s_zones.names[s_zones.count], TZ_NAME_LEN, "%s", name);
         s_zones.zones[s_zones.count++] = z;
      }
   }
}

/* tzdata's own list of old zone names, read once, so an old name resolves
 * whether the system installs it as a link, a copy, or not at all (newer
 * systems ship old names only in an optional package). */
static struct {
   pthread_once_t once;
   char (*names)[2][TZ_NAME_LEN]; /* [i][0] old, [i][1] current */
   int count;
} s_links = { .once = PTHREAD_ONCE_INIT };

static void load_links(void) {
   FILE *f = fopen(TZ_TZDATA_ZI, "r");
   if (!f)
      return;
   s_links.names = calloc(TZ_MAX_LINKS, sizeof(*s_links.names));
   char line[256];
   while (s_links.names && s_links.count < TZ_MAX_LINKS && fgets(line, sizeof(line), f)) {
      char current[TZ_NAME_LEN], old[TZ_NAME_LEN];
      if (sscanf(line, "L %63s %63s", current, old) == 2) {
         memcpy(s_links.names[s_links.count][0], old, sizeof(old));
         memcpy(s_links.names[s_links.count][1], current, sizeof(current));
         s_links.count++;
      }
   }
   fclose(f);
}

/* The current name tzdata gives old name @p name, or NULL. */
static const char *current_name(const char *name) {
   pthread_once(&s_links.once, load_links);
   for (int i = 0; i < s_links.count; i++) {
      if (strcmp(s_links.names[i][0], name) == 0)
         return s_links.names[i][1];
   }
   return NULL;
}

/* The daemon's zone when libical doesn't list it ("CET"), loaded at startup */
static tz_t *s_extra;
static char s_extra_name[TZ_NAME_LEN];

/* libical's zone named @p location, or NULL; never loads a zone it doesn't
 * list (for one it doesn't, libical opens a file of that name and keeps it,
 * so endless aliases would grow the daemon). */
static tz_t *listed_zone(const char *location) {
   pthread_once(&s_zones.once, load_zones);
   for (size_t i = 0; i < s_zones.count; i++) {
      if (strcmp(s_zones.names[i], location) == 0)
         return s_zones.zones[i];
   }
   return s_extra && strcmp(s_extra_name, location) == 0 ? s_extra : NULL;
}

/* Whether @p name is a spelling of UTC. */
static bool utc_name(const char *name) {
   static const char *const names[] = { "UTC",       "GMT",           "UCT",      "Zulu",
                                        "Universal", "Greenwich",     "Etc/UTC",  "Etc/GMT",
                                        "Etc/UCT",   "Etc/Universal", "Etc/Zulu", "Etc/Greenwich" };
   for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
      if (strcmp(name, names[i]) == 0)
         return true;
   }
   return false;
}

tz_t *tz_lookup(const char *name) {
   if (!name || !name[0])
      return NULL;
   if (utc_name(name))
      return icaltimezone_get_utc_timezone();
   tz_t *zone = listed_zone(name);
   if (zone)
      return zone;
   /* An old name ("Europe/Kiev", "US/Eastern"): tzdata names its current one. */
   const char *current = current_name(name);
   if (!current)
      return NULL;
   return utc_name(current) ? icaltimezone_get_utc_timezone() : listed_zone(current);
}

const char *tz_name(const tz_t *zone) {
   pthread_once(&s_zones.once, load_zones);
   for (size_t i = 0; zone && i < s_zones.count; i++) {
      if (s_zones.zones[i] == zone)
         return s_zones.names[i];
   }
   return zone && zone == s_extra ? s_extra_name : "UTC";
}

/* ---- The daemon's zone --------------------------------------------------- */

static tz_t *s_daemon;

/* @p name, also when libical doesn't list it but the system's zoneinfo has
 * it ("CET", "EST5EDT").  Startup only: loading an unlisted zone appends to
 * libical's list, which must not happen while other threads read it. */
static tz_t *trusted_lookup(const char *name) {
   tz_t *zone = tz_lookup(name);
   if (zone || name[0] == '/' || strstr(name, "..") || strlen(name) >= TZ_NAME_LEN)
      return zone;
   char path[PATH_MAX];
   struct stat st;
   if (snprintf(path, sizeof(path), "%s%s", TZ_ZONEINFO_DIR, name) >= (int)sizeof(path) ||
       stat(path, &st) != 0 || !S_ISREG(st.st_mode))
      return NULL;
   zone = icaltimezone_get_builtin_timezone(name);
   if (zone) {
      s_extra = zone;
      memcpy(s_extra_name, name, strlen(name) + 1);
   }
   return zone;
}

/* @p path's contents into @p buf (at most @p size bytes); its length, or 0. */
static size_t read_file(const char *path, char *buf, size_t size) {
   FILE *f = fopen(path, "rb");
   if (!f)
      return 0;
   const size_t n = fread(buf, 1, size, f);
   const bool whole = feof(f);
   fclose(f);
   return whole ? n : 0;
}

/* The zone whose file is byte for byte @p path (a copied /etc/localtime),
 * trying @p hint first; its name into @p out, or "". */
static void zone_matching_file(const char *path, const char *hint, char *out, size_t size) {
   out[0] = '\0';
   char *want = malloc(TZ_MAX_ZONE_FILE), *have = malloc(TZ_MAX_ZONE_FILE);
   const size_t n = want ? read_file(path, want, TZ_MAX_ZONE_FILE) : 0;
   pthread_once(&s_zones.once, load_zones);
   for (size_t i = 0; n && have && i <= s_zones.count && !out[0]; i++) {
      const char *name = i == 0 ? hint : s_zones.names[i - 1];
      char file[PATH_MAX];
      if (!name || !name[0] ||
          snprintf(file, sizeof(file), "%s%s", TZ_ZONEINFO_DIR, name) >= (int)sizeof(file))
         continue;
      if (read_file(file, have, TZ_MAX_ZONE_FILE) == n && memcmp(want, have, n) == 0)
         snprintf(out, size, "%s", name);
   }
   free(want);
   free(have);
}

/* The zone name a zone file's path gives ("…/zoneinfo/Europe/Paris"), after
 * following links; else, for a copy, the zone with the same contents. */
static void zone_of_file(const char *path, const char *hint, char *out, size_t size) {
   char real[PATH_MAX];
   const char *name = realpath(path, real) ? strstr(real, "zoneinfo/") : NULL;
   if (name)
      snprintf(out, size, "%s", name + strlen("zoneinfo/"));
   else
      zone_matching_file(path, hint, out, size);
}

/* The system's zone name into @p out, as the C library finds it: TZ (a
 * name, or a file after ':'), else /etc/localtime; "" when none is found
 * (UTC, as for the C library).  /etc/timezone only helps name a copied
 * /etc/localtime. */
static void system_zone_name(char *out, size_t size) {
   out[0] = '\0';
   char hint[TZ_NAME_LEN] = "";
   FILE *f = fopen("/etc/timezone", "r");
   if (f) {
      if (fgets(hint, sizeof(hint), f))
         hint[strcspn(hint, " \t\r\n")] = '\0';
      fclose(f);
   }
   const char *tz = getenv("TZ");
   if (tz && strcmp(tz, ":") != 0) { /* a bare ':' is the default, as unset */
      if (*tz == ':')
         tz++;
      if (tz[0] == '/')
         zone_of_file(tz, hint, out, size);
      else
         snprintf(out, size, "%s", tz);
      return;
   }
   zone_of_file("/etc/localtime", hint, out, size);
}

/* Whether the C library's offsets for this process (now and half a year on)
 * are @p zone's. */
static bool libc_agrees(const tz_t *zone) {
   const time_t now = time(NULL);
   for (int i = 0; i < 2; i++) {
      const time_t t = now + (time_t)i * 182 * TZ_DAY;
      struct tm tm;
      localtime_r(&t, &tm);
      if (tm.tm_gmtoff != tz_wall(zone, t) - t)
         return false;
   }
   return true;
}

void tz_daemon_init(const char *configured) {
   char name[PATH_MAX];
   if (configured && configured[0])
      snprintf(name, sizeof(name), "%s", configured);
   else
      system_zone_name(name, sizeof(name));

   tz_t *zone = name[0] ? trusted_lookup(name) : NULL;
   if (!zone) {
      if (name[0])
         OLOG_WARNING("Timezone '%s' isn't a zone name DAWN can use; reading times as UTC. "
                      "Set [localization] timezone to a name like America/New_York.",
                      name);
      zone = icaltimezone_get_utc_timezone();
   }
   if (!libc_agrees(zone))
      OLOG_WARNING("Timezone %s doesn't match this system's clock settings; set "
                   "[localization] timezone to the zone you want.",
                   tz_name(zone));
   s_daemon = zone;
   OLOG_INFO("Timezone for clock times: %s", tz_name(zone));
}

tz_t *tz_daemon(void) {
   return s_daemon ? s_daemon : icaltimezone_get_utc_timezone();
}

/* ---- Conversion ---------------------------------------------------------- */

time_t tz_wall(const tz_t *zone, time_t instant) {
   if (!zone || zone == icaltimezone_get_utc_timezone())
      return instant;
   const icaltimetype t = icaltime_from_timet_with_zone(instant, 0, (tz_t *)zone);
   struct tm tm = { 0 };
   tm.tm_year = t.year - 1900;
   tm.tm_mon = t.month - 1;
   tm.tm_mday = t.day;
   tm.tm_hour = t.hour;
   tm.tm_min = t.minute;
   tm.tm_sec = t.second;
   return timegm(&tm);
}

/* The instants whose reading in @p zone is @p wall: with the offsets a day
 * either side (no zone changes twice in a day), each candidate whose reading
 * comes back.  Both in a repeated hour; neither in a skipped one, where
 * @p earlier gets the offset before the change, which carries it forward by
 * the gap.  @p later is -1 when the reading happens once. */
static void from_wall(const tz_t *zone, time_t wall, time_t *earlier, time_t *later) {
   *later = -1;
   if (!zone || zone == icaltimezone_get_utc_timezone()) {
      *earlier = wall;
      return;
   }
   const time_t before = wall - TZ_DAY, after = wall + TZ_DAY;
   const time_t a = wall - (tz_wall(zone, before) - before);
   const time_t b = wall - (tz_wall(zone, after) - after);
   if (a == b) { /* no change near: the reading happens once */
      *earlier = a;
      return;
   }
   const bool a_ok = tz_wall(zone, a) == wall, b_ok = tz_wall(zone, b) == wall;
   if (a_ok && b_ok) {
      *earlier = a < b ? a : b;
      *later = a < b ? b : a;
   } else {
      *earlier = b_ok ? b : a;
   }
}

time_t tz_from_wall(const tz_t *zone, time_t wall) {
   time_t earlier, later;
   from_wall(zone, wall, &earlier, &later);
   return earlier;
}

void tz_local(const tz_t *zone, time_t instant, struct tm *out) {
   const time_t wall = tz_wall(zone, instant);
   gmtime_r(&wall, out);
   out->tm_gmtoff = (long)(wall - instant);
   out->tm_zone = NULL;
   out->tm_isdst = -1; /* nothing reads it; finding it costs a second conversion */
}

time_t tz_mktime(const tz_t *zone, const struct tm *tm) {
   struct tm fields = *tm;
   fields.tm_isdst = 0;
   return tz_from_wall(zone, timegm(&fields));
}

/* ---- Parsing ------------------------------------------------------------- */

/* The next instant at or after @p now whose reading in @p zone is @p clock
 * seconds past midnight: today (either pass of a repeated hour), else
 * tomorrow, stepping whole days on the clock. */
static time_t next_clock(const tz_t *zone, time_t now, time_t clock) {
   const time_t today = tz_wall(zone, now) / TZ_DAY * TZ_DAY;
   time_t earlier, later;
   from_wall(zone, today + clock, &earlier, &later);
   if (earlier > now)
      return earlier;
   if (later > now)
      return later;
   return tz_from_wall(zone, today + TZ_DAY + clock);
}

bool tz_parse(const char *text, const tz_t *zone, time_t *out) {
   iso8601_fields_t f;
   if (!out || !iso8601_fields(text, &f))
      return false;

   if (f.time_only) {
      *out = next_clock(zone, time(NULL), (time_t)f.hour * 3600 + (time_t)f.min * 60);
      return true;
   }

   struct tm tm = { 0 };
   tm.tm_year = f.year - 1900;
   tm.tm_mon = f.month - 1;
   tm.tm_mday = f.day;
   tm.tm_hour = f.hour;
   tm.tm_min = f.min;
   tm.tm_sec = f.sec;
   if (f.has_offset) {
      *out = timegm(&tm) - f.offset_sec;
      return true;
   }
   *out = tz_mktime(zone, &tm);
   return true;
}
