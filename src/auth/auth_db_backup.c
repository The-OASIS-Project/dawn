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
 * Pre-upgrade database backup.
 *
 * Before any schema migration, snapshot the database via `VACUUM INTO` (a
 * consistent single-file copy that correctly captures WAL state, unlike a raw
 * filesystem copy).  The backup directory and base filename are DERIVED from the
 * live db_path — nothing here assumes a fixed name like "auth.db", so renaming
 * or relocating the database needs no change here.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "auth/auth_db_internal.h"
#include "utils/string_utils.h"
#undef AUTH_DB_INTERNAL_ALLOWED

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "logging.h"

/* Keep the most recent N pre-upgrade backups per database; prune older ones. */
#define AUTH_DB_BACKUP_KEEP 10

typedef struct {
   char name[256];
   time_t mtime;
} backup_entry_t;

static int cmp_backup_mtime_asc(const void *a, const void *b) {
   time_t ma = ((const backup_entry_t *)a)->mtime;
   time_t mb = ((const backup_entry_t *)b)->mtime;
   return (ma < mb) ? -1 : (ma > mb) ? 1 : 0;
}

/* Delete all but the newest AUTH_DB_BACKUP_KEEP files in @backup_dir whose name
 * starts with @prefix (this database's backups).  Best-effort; never fatal. */
static void prune_old_backups(const char *backup_dir, const char *prefix) {
   DIR *d = opendir(backup_dir);
   if (!d) {
      return;
   }
   backup_entry_t entries[128];
   int n = 0;
   size_t prefix_len = strlen(prefix);
   struct dirent *e;
   while ((e = readdir(d)) != NULL && n < (int)(sizeof(entries) / sizeof(entries[0]))) {
      if (strncmp(e->d_name, prefix, prefix_len) != 0) {
         continue;
      }
      char full[1024];
      snprintf(full, sizeof(full), "%s/%s", backup_dir, e->d_name);
      struct stat st;
      if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
         continue;
      }
      safe_strscpy(entries[n].name, e->d_name);
      entries[n].name[sizeof(entries[n].name) - 1] = '\0';
      entries[n].mtime = st.st_mtime;
      n++;
   }
   closedir(d);

   if (n <= AUTH_DB_BACKUP_KEEP) {
      return;
   }
   qsort(entries, (size_t)n, sizeof(entries[0]), cmp_backup_mtime_asc);
   for (int i = 0; i < n - AUTH_DB_BACKUP_KEEP; i++) {
      char full[1024];
      int m = snprintf(full, sizeof(full), "%s/%s", backup_dir, entries[i].name);
      if (m < 0 || (size_t)m >= sizeof(full)) {
         continue; /* path too long — never unlink() a truncated path */
      }
      if (unlink(full) == 0) {
         OLOG_INFO("auth_db: pruned old backup %s", entries[i].name);
      }
   }
}

int auth_db_backup_before_upgrade(const char *db_path, int from_version) {
   /* Nothing to back up for an in-memory or empty path (and the caller only
    * invokes this for a real on-disk upgrade anyway). */
   if (!db_path || db_path[0] == '\0' || strcmp(db_path, ":memory:") == 0) {
      return AUTH_DB_SUCCESS;
   }

   /* Derive directory + base filename from db_path — no hardcoded name. */
   const char *slash = strrchr(db_path, '/');
   const char *base = slash ? slash + 1 : db_path;
   if (base[0] == '\0') {
      OLOG_ERROR("auth_db: backup skipped — db_path has no filename: %s", db_path);
      return AUTH_DB_FAILURE;
   }
   char dir[512];
   if (slash) {
      size_t dlen = (size_t)(slash - db_path);
      if (dlen >= sizeof(dir)) {
         OLOG_ERROR("auth_db: backup skipped — db_path too long");
         return AUTH_DB_FAILURE;
      }
      memcpy(dir, db_path, dlen);
      dir[dlen] = '\0';
      if (dir[0] == '\0') {
         dir[0] = '/'; /* db_path was "/<file>" — directory is root */
         dir[1] = '\0';
      }
   } else {
      dir[0] = '.';
      dir[1] = '\0';
   }

   char backup_dir[768];
   snprintf(backup_dir, sizeof(backup_dir), "%s/backups", dir);
   /* Owner-only (0700): the backups hold full credential snapshots (password
    * hashes, session tokens, encrypted blobs). */
   if (mkdir(backup_dir, 0700) != 0 && errno != EEXIST) {
      OLOG_ERROR("auth_db: cannot create backup dir %s: %s", backup_dir, strerror(errno));
      return AUTH_DB_FAILURE;
   }
   /* Tighten an existing dir too (older builds created it 0750). Best-effort. */
   if (chmod(backup_dir, 0700) != 0) {
      OLOG_WARNING("auth_db: could not chmod backup dir %s to 0700: %s", backup_dir,
                   strerror(errno));
   }

   char ts[24];
   time_t now = time(NULL);
   struct tm tm_now;
   localtime_r(&now, &tm_now);
   strftime(ts, sizeof(ts), "%Y%m%dT%H%M%S", &tm_now);

   /* "<base>.v<from>-<ts>.bak"; prune matches all "<base>.v*" for this DB.
    * Treat truncation as failure: a clipped path would back up to the wrong file
    * (or break prune matching), defeating the safety guarantee. */
   char prefix[300];
   if (snprintf(prefix, sizeof(prefix), "%s.v", base) >= (int)sizeof(prefix)) {
      OLOG_ERROR("auth_db: backup skipped — db filename too long for prefix buffer");
      return AUTH_DB_FAILURE;
   }
   char backup_path[1200];
   if (snprintf(backup_path, sizeof(backup_path), "%s/%s.v%d-%s.bak", backup_dir, base,
                from_version, ts) >= (int)sizeof(backup_path)) {
      OLOG_ERROR("auth_db: backup skipped — backup path too long");
      return AUTH_DB_FAILURE;
   }

   /* %Q quotes + SQL-escapes the path (VACUUM takes a string literal, not a
    * bound parameter). */
   char *sql = sqlite3_mprintf("VACUUM INTO %Q", backup_path);
   if (!sql) {
      OLOG_ERROR("auth_db: backup OOM building statement");
      return AUTH_DB_FAILURE;
   }
   char *errmsg = NULL;
   int rc = sqlite3_exec(s_db.db, sql, NULL, NULL, &errmsg);
   sqlite3_free(sql);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: pre-upgrade backup (VACUUM INTO) failed: %s",
                 errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }

   /* Restrict the snapshot to owner-only — it is a full credential DB copy.  The
    * 0700 dir already blocks others; this closes the gap if the dir is ever
    * relaxed and removes the brief umask-default window. */
   if (chmod(backup_path, 0600) != 0) {
      OLOG_WARNING("auth_db: could not chmod backup %s to 0600: %s", backup_path, strerror(errno));
   }

   OLOG_INFO("auth_db: backed up pre-upgrade database (v%d) to %s", from_version, backup_path);
   prune_old_backups(backup_dir, prefix);
   return AUTH_DB_SUCCESS;
}
