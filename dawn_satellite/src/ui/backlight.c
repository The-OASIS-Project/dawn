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
 * Sysfs backlight abstraction for Pi 7" touchscreen
 */

#include "ui/backlight.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SYSFS_BASE "/sys/class/backlight"

static char brightness_path[320]; /* Full path: ".../brightness" (cached at init) */
static int max_brightness = 0;
static bool bl_available = false;
static int bl_fd = -1;    /* Held open between backlight_open/close for low-latency writes */
static int last_raw = -1; /* Last written raw value; skip redundant sysfs writes */

static int read_int(const char *path) {
   FILE *fp = fopen(path, "r");
   if (!fp)
      return -1;
   int val = 0;
   if (fscanf(fp, "%d", &val) != 1)
      val = -1;
   fclose(fp);
   return val;
}

int backlight_init(void) {
   /* Probe well-known paths first */
   static const char *candidates[] = {
      SYSFS_BASE "/10-0045",       /* Official Pi touchscreen */
      SYSFS_BASE "/rpi_backlight", /* Older Pi firmware */
   };
   for (int i = 0; i < 2; i++) {
      char mbp[320];
      snprintf(mbp, sizeof(mbp), "%s/max_brightness", candidates[i]);
      int mb = read_int(mbp);
      if (mb > 0) {
         snprintf(brightness_path, sizeof(brightness_path), "%s/brightness", candidates[i]);
         max_brightness = mb;
         bl_available = true;
         return 0;
      }
   }

   /* Fallback: first entry in /sys/class/backlight/ */
   DIR *dir = opendir(SYSFS_BASE);
   if (dir) {
      struct dirent *ent;
      while ((ent = readdir(dir)) != NULL) {
         if (ent->d_name[0] == '.')
            continue;
         char mbp[320];
         snprintf(mbp, sizeof(mbp), SYSFS_BASE "/%s/max_brightness", ent->d_name);
         int mb = read_int(mbp);
         if (mb > 0) {
            snprintf(brightness_path, sizeof(brightness_path), SYSFS_BASE "/%s/brightness",
                     ent->d_name);
            max_brightness = mb;
            bl_available = true;
            closedir(dir);
            return 0;
         }
      }
      closedir(dir);
   }

   return 1; /* No backlight found */
}

int backlight_get(void) {
   if (!bl_available)
      return 100;
   int raw = read_int(brightness_path);
   if (raw < 0)
      return 100;
   return (raw * 100 + max_brightness / 2) / max_brightness;
}

void backlight_set(int pct) {
   if (!bl_available)
      return;
   if (pct < 10)
      pct = 10; /* Floor: prevent black screen */
   if (pct > 100)
      pct = 100;
   int raw = (pct * max_brightness + 50) / 100;
   if (raw == last_raw)
      return; /* Skip redundant sysfs write */
   last_raw = raw;

   if (bl_fd >= 0) {
      /* Fast path: pre-opened fd from backlight_open() */
      char buf[16];
      int len = snprintf(buf, sizeof(buf), "%d", raw);
      lseek(bl_fd, 0, SEEK_SET);
      if (write(bl_fd, buf, (size_t)len) < 0)
         last_raw = -1; /* Force retry next call */
   } else {
      /* Fallback: open/write/close */
      FILE *fp = fopen(brightness_path, "w");
      if (!fp)
         return;
      fprintf(fp, "%d", raw);
      fclose(fp);
   }
}

bool backlight_available(void) {
   return bl_available;
}

void backlight_open(void) {
   if (!bl_available || bl_fd >= 0)
      return;
   bl_fd = open(brightness_path, O_WRONLY);
}

void backlight_close(void) {
   if (bl_fd >= 0) {
      close(bl_fd);
      bl_fd = -1;
   }
}
