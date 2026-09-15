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
 * HTTP file download utility — downloads a URL to a temporary file
 */

#include "audio/http_download.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "logging.h"

/* Write callback that streams directly to a FILE* */
static size_t write_to_file(void *contents, size_t size, size_t nmemb, void *userp) {
   FILE *fp = (FILE *)userp;
   return fwrite(contents, size, nmemb, fp);
}

int http_download_to_temp(CURL *curl,
                          const char *url,
                          struct curl_slist *headers,
                          const char *suffix,
                          const char *prefix,
                          int64_t max_size,
                          char *out_path,
                          size_t out_path_size) {
   if (!curl || !url || !out_path || out_path_size == 0) {
      return 1;
   }

   /* Build temp file template: {prefix}XXXXXX{suffix} */
   const char *sfx = suffix ? suffix : "";
   int written = snprintf(out_path, out_path_size, "%sXXXXXX%s", prefix ? prefix : "/tmp/dl_", sfx);
   if (written < 0 || (size_t)written >= out_path_size) {
      OLOG_ERROR("http_download: path buffer too small");
      return 1;
   }

   /* Create temp file with suffix */
   int suffixlen = (int)strlen(sfx);
   int fd = mkstemps(out_path, suffixlen);
   if (fd < 0) {
      OLOG_ERROR("http_download: mkstemps failed: %s", strerror(errno));
      return 1;
   }

   /* Restrictive permissions */
   if (fchmod(fd, 0600) != 0) {
      OLOG_WARNING("http_download: fchmod failed: %s", strerror(errno));
   }

   FILE *fp = fdopen(fd, "wb");
   if (!fp) {
      OLOG_ERROR("http_download: fdopen failed: %s", strerror(errno));
      close(fd);
      unlink(out_path);
      return 1;
   }

   /* Configure CURL */
   curl_easy_reset(curl);
   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L); /* No redirects — prevents SSRF */
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
   curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
   curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L); /* Hard ceiling prevents hung mutex */
   curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
   curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

   if (max_size > 0) {
      curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)max_size);
   }

   if (headers) {
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   }

   CURLcode res = curl_easy_perform(curl);

   fclose(fp); /* Also closes fd */

   if (res != CURLE_OK) {
      if (res == CURLE_FILESIZE_EXCEEDED) {
         OLOG_ERROR("http_download: file exceeds size limit (%lld bytes)", (long long)max_size);
      } else {
         OLOG_ERROR("http_download: CURL error: %s", curl_easy_strerror(res));
      }
      unlink(out_path);
      return 1;
   }

   /* Check HTTP response code */
   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
   if (http_code < 200 || http_code >= 300) {
      OLOG_ERROR("http_download: HTTP %ld for %s", http_code, url);
      unlink(out_path);
      return 1;
   }

   /* Log large downloads. CURLINFO_SIZE_DOWNLOAD_T (curl_off_t) supersedes the
    * deprecated double CURLINFO_SIZE_DOWNLOAD (since libcurl 7.55). */
   curl_off_t dl_size = 0;
   curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &dl_size);
   if (dl_size > 100 * 1024 * 1024) {
      OLOG_WARNING("http_download: large file downloaded (%.1f MB)",
                   (double)dl_size / (1024 * 1024));
   }

   return 0;
}
