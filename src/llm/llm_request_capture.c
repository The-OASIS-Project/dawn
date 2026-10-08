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
 * Request capture for the LLM quality suite: the next requests one user's
 * turns send, written as JSON files with credentials redacted.
 */

#include "llm/llm_request_capture.h"

#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "core/session_manager.h"
#include "dawn_error.h"
#include "logging.h"

/* A request body nests deeper than json-c's default 32 (tool schemas). */
#define CAPTURE_JSON_DEPTH 128

/* LEAF: held only to read or update the arming; never across file I/O. */
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool s_armed;
static int s_user_id;
static int s_remaining;
static int s_seq;
static int s_dir_fd = -1;
static time_t s_expires;

static void set_err(char *err, size_t err_len, const char *msg) {
   if (err && err_len > 0) {
      snprintf(err, err_len, "%s", msg);
   }
}

/* Whether directory @p fd holds no entries (it doesn't consume @p fd). */
static bool dir_is_empty(int fd) {
   const int copy = fcntl(fd, F_DUPFD_CLOEXEC, 0);
   DIR *d = copy >= 0 ? fdopendir(copy) : NULL;
   if (!d) {
      if (copy >= 0) {
         close(copy);
      }
      return false;
   }
   bool empty = true;
   for (struct dirent *e; (e = readdir(d)) != NULL;) {
      if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
         empty = false;
         break;
      }
   }
   closedir(d);
   return empty;
}

/* Close the armed directory and clear the arming.  Call with s_mutex held. */
static void disarm_locked(void) {
   atomic_store(&s_armed, false);
   s_remaining = 0;
   if (s_dir_fd >= 0) {
      close(s_dir_fd);
      s_dir_fd = -1;
   }
}

int llm_request_capture_arm(int user_id, const char *dir, int count, char *err, size_t err_len) {
   if (user_id <= 0) {
      set_err(err, err_len, "no such user");
      return FAILURE;
   }
   if (count < 1 || count > LLM_CAPTURE_COUNT_MAX) {
      set_err(err, err_len, "count must be 1-50");
      return FAILURE;
   }
   if (!dir || dir[0] != '/' || strlen(dir) > LLM_CAPTURE_DIR_MAX) {
      set_err(err, err_len, "directory must be an absolute path (at most 200 bytes)");
      return FAILURE;
   }
   /* Opened once, not followed through a symlink, and written by fd: the
    * directory checked here is the one written into. */
   const int fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   struct stat st;
   if (fd < 0 || fstat(fd, &st) != 0) {
      if (fd >= 0) {
         close(fd);
      }
      set_err(err, err_len, "directory does not exist (or is a symlink)");
      return FAILURE;
   }
   if (st.st_uid != geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH))) {
      close(fd);
      set_err(err, err_len,
              "directory must be owned by the daemon's user and writable by no one "
              "else");
      return FAILURE;
   }
   if (!dir_is_empty(fd)) {
      close(fd);
      set_err(err, err_len, "directory must be empty");
      return FAILURE;
   }
   pthread_mutex_lock(&s_mutex);
   disarm_locked();
   s_dir_fd = fd;
   s_user_id = user_id;
   s_remaining = count;
   s_seq = 0;
   s_expires = time(NULL) + LLM_CAPTURE_TTL_SEC;
   atomic_store(&s_armed, true);
   pthread_mutex_unlock(&s_mutex);
   OLOG_INFO("llm_capture: armed for user %d, %d request(s) to %s, for %d min", user_id, count, dir,
             LLM_CAPTURE_TTL_SEC / 60);
   return SUCCESS;
}

void llm_request_capture_disarm(void) {
   pthread_mutex_lock(&s_mutex);
   disarm_locked();
   pthread_mutex_unlock(&s_mutex);
}

int llm_request_capture_remaining(void) {
   pthread_mutex_lock(&s_mutex);
   if (atomic_load(&s_armed) && time(NULL) >= s_expires) {
      disarm_locked();
   }
   const int n = atomic_load(&s_armed) ? s_remaining : 0;
   pthread_mutex_unlock(&s_mutex);
   return n;
}

/* Whether a header name carries a credential. */
static bool is_secret_header(const char *line, size_t name_len) {
   static const char *const names[] = { "authorization",  "x-api-key", "api-key",
                                        "x-goog-api-key", "cookie",    NULL };
   for (int i = 0; names[i]; i++) {
      if (strlen(names[i]) == name_len && strncasecmp(line, names[i], name_len) == 0) {
         return true;
      }
   }
   return false;
}

/* Whether query parameter @p p (at "name=...") names a credential. */
static bool is_secret_param(const char *p) {
   static const char *const names[] = { "key=", "api_key=", "access_token=", "token=", NULL };
   for (int i = 0; names[i]; i++) {
      if (strncmp(p, names[i], strlen(names[i])) == 0) {
         return true;
      }
   }
   return false;
}

/* @p url with userinfo ("user:pass@") and credential query values replaced. */
static void redact_url(const char *url, char *out, size_t out_len) {
   size_t o = 0;
#define PUT(c)             \
   do {                    \
      if (o + 1 < out_len) \
         out[o++] = (c);   \
   } while (0)
   const char *p = url;
   const char *scheme = strstr(url, "://");
   if (scheme) {
      const char *host = scheme + 3;
      const char *slash = strchr(host, '/');
      const char *at = strchr(host, '@');
      for (; p < host; p++) {
         PUT(*p);
      }
      if (at && (!slash || at < slash)) {
         for (const char *r = "[REDACTED]@"; *r; r++) {
            PUT(*r);
         }
         p = at + 1;
      }
   }
   bool in_query = false;
   while (*p) {
      PUT(*p);
      if (*p == '?' || (in_query && *p == '&')) {
         in_query = true;
         if (is_secret_param(p + 1)) {
            const char *eq = strchr(p + 1, '=');
            for (const char *q = p + 1; q <= eq; q++) {
               PUT(*q);
            }
            for (const char *r = "[REDACTED]"; *r; r++) {
               PUT(*r);
            }
            p = eq + 1;
            while (*p && *p != '&') {
               p++;
            }
            continue;
         }
      }
      p++;
   }
   out[o] = '\0';
#undef PUT
}

/* Claim the next file slot when armed for the calling session's user.  On
 * success @p fd_out is a duplicate of the directory fd, the caller's to close. */
static bool claim_slot(int *seq_out, int *fd_out) {
   if (!atomic_load(&s_armed)) {
      return false;
   }
   session_t *session = session_get_command_context();
   if (!session) {
      return false; /* a background call (extraction, compaction): not a turn */
   }
   const int user_id = session_effective_user_id(session);
   bool claimed = false;
   pthread_mutex_lock(&s_mutex);
   if (atomic_load(&s_armed) && time(NULL) >= s_expires) {
      OLOG_INFO("llm_capture: arming expired; stopped");
      disarm_locked();
   }
   if (atomic_load(&s_armed) && user_id == s_user_id && s_remaining > 0) {
      *fd_out = fcntl(s_dir_fd, F_DUPFD_CLOEXEC, 0);
      if (*fd_out >= 0) {
         *seq_out = ++s_seq;
         claimed = true;
         if (--s_remaining == 0) {
            disarm_locked();
         }
      }
   }
   pthread_mutex_unlock(&s_mutex);
   return claimed;
}

void llm_request_capture(const char *provider,
                         const char *url,
                         const struct curl_slist *headers,
                         const char *body) {
   int seq = 0;
   int dir_fd = -1;
   if (!provider || !url || !body || !claim_slot(&seq, &dir_fd)) {
      return;
   }

   json_object *root = json_object_new_object();
   json_object_object_add(root, "provider", json_object_new_string(provider));
   char safe_url[4096];
   redact_url(url, safe_url, sizeof(safe_url));
   json_object_object_add(root, "url", json_object_new_string(safe_url));
   json_object_object_add(root, "captured_at", json_object_new_int64((int64_t)time(NULL)));
   json_object *hdrs = json_object_new_array();
   for (const struct curl_slist *h = headers; h; h = h->next) {
      const char *colon = h->data ? strchr(h->data, ':') : NULL;
      if (colon && is_secret_header(h->data, (size_t)(colon - h->data))) {
         char line[256];
         snprintf(line, sizeof(line), "%.*s: [REDACTED]", (int)(colon - h->data), h->data);
         json_object_array_add(hdrs, json_object_new_string(line));
      } else if (h->data) {
         json_object_array_add(hdrs, json_object_new_string(h->data));
      }
   }
   json_object_object_add(root, "headers", hdrs);
   json_tokener *tok = json_tokener_new_ex(CAPTURE_JSON_DEPTH);
   json_object *parsed = tok ? json_tokener_parse_ex(tok, body, (int)strlen(body)) : NULL;
   if (tok) {
      json_tokener_free(tok);
   }
   json_object_object_add(root, "body", parsed ? parsed : json_object_new_string(body));

   char name[64];
   snprintf(name, sizeof(name), "%03d-%s.json", seq, provider);
   const int fd = openat(dir_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
   close(dir_fd);
   if (fd < 0) {
      OLOG_WARNING("llm_capture: cannot create %s: %s", name, strerror(errno));
      json_object_put(root);
      return;
   }
   const char *text = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY |
                                                               JSON_C_TO_STRING_NOSLASHESCAPE);
   const size_t len = strlen(text);
   const ssize_t written = write(fd, text, len);
   close(fd);
   if (written != (ssize_t)len) {
      OLOG_WARNING("llm_capture: short write to %s", name);
   } else {
      OLOG_INFO("llm_capture: wrote %s (%zu bytes)", name, len);
   }
   json_object_put(root);
}
