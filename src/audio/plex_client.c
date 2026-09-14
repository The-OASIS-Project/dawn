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
 * Plex Media Server REST API client for music library browsing and streaming.
 *
 * All API responses use JSON (via Accept: application/json header).
 * Two persistent CURL handles are maintained: one for API queries, one for downloads.
 * Token is NEVER stored in path fields — only used at HTTP request time.
 */

#include "audio/plex_client.h"

#include <curl/curl.h>
#include <glob.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include "audio/http_download.h"
#include "config/dawn_config.h"
#include "core/curl_buffer.h"
#include "dawn_error.h"
#include "logging.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Static State
 * ============================================================================= */

static CURL *s_api_curl = NULL;      /* Persistent handle for API JSON queries */
static CURL *s_download_curl = NULL; /* Persistent handle for file downloads */
static int s_section_id = 0;         /* Cached music section ID (0 = not discovered) */

/* Mutex protection — CURL handles are not thread-safe. Multiple WebUI sessions
 * and LLM tool threads can invoke Plex API calls concurrently. */
static pthread_mutex_t s_api_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_download_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Cached API request headers — rebuilt on token/config change */
static struct curl_slist *s_cached_headers = NULL;
static pthread_mutex_t s_headers_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * Internal Helpers
 * ============================================================================= */

/** Build base URL from config: http(s)://host:port */
static int build_base_url(char *buf, size_t buf_size) {
   const plex_config_t *plex = &g_config.music.plex;
   const char *scheme = plex->ssl ? "https" : "http";
   int n = snprintf(buf, buf_size, "%s://%s:%d", scheme, plex->host, plex->port);
   if (n < 0 || (size_t)n >= buf_size) {
      OLOG_ERROR("Plex: base URL too long");
      return FAILURE;
   }
   return SUCCESS;
}

/** Build and cache common request headers (Accept JSON + auth token).
 * Returns a pointer to the cached slist — caller must NOT free it.
 * Thread-safe: protected by s_headers_mutex. */
static struct curl_slist *get_api_headers(void) {
   pthread_mutex_lock(&s_headers_mutex);

   if (s_cached_headers) {
      pthread_mutex_unlock(&s_headers_mutex);
      return s_cached_headers;
   }

   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, "Accept: application/json");

   if (g_secrets.plex_token[0]) {
      char auth[320];
      snprintf(auth, sizeof(auth), "X-Plex-Token: %s", g_secrets.plex_token);
      headers = curl_slist_append(headers, auth);
   }

   const char *client_id = g_config.music.plex.client_identifier;
   if (client_id[0]) {
      char id_header[128];
      snprintf(id_header, sizeof(id_header), "X-Plex-Client-Identifier: %s", client_id);
      headers = curl_slist_append(headers, id_header);
   }

   headers = curl_slist_append(headers, "X-Plex-Product: DAWN");
   headers = curl_slist_append(headers, "X-Plex-Version: 1.0");
   headers = curl_slist_append(headers, "X-Plex-Platform: Linux");

   s_cached_headers = headers;
   pthread_mutex_unlock(&s_headers_mutex);
   return headers;
}

/** Invalidate cached headers (call after token or config change) */
static void invalidate_api_headers(void) {
   pthread_mutex_lock(&s_headers_mutex);
   if (s_cached_headers) {
      curl_slist_free_all(s_cached_headers);
      s_cached_headers = NULL;
   }
   pthread_mutex_unlock(&s_headers_mutex);
}

/**
 * Validate that a Plex rating key is numeric-only.
 * Prevents URL path injection via crafted WebSocket messages.
 */
static bool is_valid_rating_key(const char *key) {
   if (!key || !key[0])
      return false;
   for (const char *p = key; *p; p++) {
      if (*p < '0' || *p > '9')
         return false;
   }
   return true;
}

/** Perform an API GET request and parse JSON response (mutex-protected) */
static json_object *api_get(const char *endpoint) {
   if (!s_api_curl) {
      OLOG_ERROR("Plex: client not initialized");
      return NULL;
   }

   char base_url[512];
   if (build_base_url(base_url, sizeof(base_url)) != SUCCESS) {
      return NULL;
   }

   char url[1024];
   int n = snprintf(url, sizeof(url), "%s%s", base_url, endpoint);
   if (n < 0 || (size_t)n >= sizeof(url)) {
      OLOG_ERROR("Plex: URL too long for endpoint %s", endpoint);
      return NULL;
   }

   struct curl_slist *headers = get_api_headers();

   curl_buffer_t buffer;
   curl_buffer_init_with_max(&buffer, PLEX_API_MAX_RESPONSE);

   pthread_mutex_lock(&s_api_mutex);

   curl_easy_reset(s_api_curl);
   curl_easy_setopt(s_api_curl, CURLOPT_URL, url);
   curl_easy_setopt(s_api_curl, CURLOPT_HTTPHEADER, headers);
   curl_easy_setopt(s_api_curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(s_api_curl, CURLOPT_WRITEDATA, &buffer);
   curl_easy_setopt(s_api_curl, CURLOPT_FOLLOWLOCATION, 0L);
   curl_easy_setopt(s_api_curl, CURLOPT_CONNECTTIMEOUT, 10L);
   curl_easy_setopt(s_api_curl, CURLOPT_TIMEOUT, 30L);
   curl_easy_setopt(s_api_curl, CURLOPT_NOSIGNAL, 1L);
   curl_easy_setopt(s_api_curl, CURLOPT_TCP_KEEPALIVE, 1L);

   const plex_config_t *plex = &g_config.music.plex;
   if (plex->ssl && !plex->ssl_verify) {
      /* Disable peer certificate verification (self-signed OK) but keep
       * hostname verification — cert CN/SAN must still match the host */
      curl_easy_setopt(s_api_curl, CURLOPT_SSL_VERIFYPEER, 0L);
      curl_easy_setopt(s_api_curl, CURLOPT_SSL_VERIFYHOST, 2L);
   }

   CURLcode res = curl_easy_perform(s_api_curl);

   long http_code = 0;
   if (res == CURLE_OK) {
      curl_easy_getinfo(s_api_curl, CURLINFO_RESPONSE_CODE, &http_code);
   }

   pthread_mutex_unlock(&s_api_mutex);

   /* headers are cached — do not free */

   if (res != CURLE_OK) {
      OLOG_ERROR("Plex API: CURL error for %s: %s", endpoint, curl_easy_strerror(res));
      curl_buffer_free(&buffer);
      return NULL;
   }

   if (http_code == 401) {
      OLOG_ERROR("Plex API: authentication failed (401) — check plex_token in secrets.toml");
      curl_buffer_free(&buffer);
      return NULL;
   }
   if (http_code < 200 || http_code >= 300) {
      OLOG_ERROR("Plex API: HTTP %ld for %s", http_code, endpoint);
      curl_buffer_free(&buffer);
      return NULL;
   }

   if (!buffer.data || buffer.size == 0) {
      OLOG_ERROR("Plex API: empty response for %s", endpoint);
      curl_buffer_free(&buffer);
      return NULL;
   }

   json_object *root = json_tokener_parse(buffer.data);
   curl_buffer_free(&buffer);

   if (!root) {
      OLOG_ERROR("Plex API: invalid JSON for %s", endpoint);
      return NULL;
   }

   return root;
}

/** Safe JSON string extraction */
static const char *json_get_string(json_object *obj, const char *key) {
   json_object *val;
   if (json_object_object_get_ex(obj, key, &val) && json_object_is_type(val, json_type_string)) {
      return json_object_get_string(val);
   }
   return "";
}

/** Safe JSON int extraction */
static int json_get_int(json_object *obj, const char *key) {
   json_object *val;
   if (json_object_object_get_ex(obj, key, &val) && json_object_is_type(val, json_type_int)) {
      return json_object_get_int(val);
   }
   return 0;
}

/**
 * Extract file extension from a Part.key path.
 * e.g., "/library/parts/9877/1234567890/file.flac" → ".flac"
 */
static const char *extract_extension(const char *path) {
   if (!path)
      return "";
   const char *dot = strrchr(path, '.');
   if (!dot)
      return "";
   /* Sanity check — no dots after a '?' (shouldn't happen in Part.key, but be safe) */
   const char *q = strchr(path, '?');
   if (q && dot > q)
      return "";
   return dot;
}

/**
 * Get the Part.key from a Plex track metadata item.
 * Navigates: item.Media[0].Part[0].key
 */
static const char *get_part_key(json_object *item) {
   json_object *media_arr;
   if (!json_object_object_get_ex(item, "Media", &media_arr) ||
       !json_object_is_type(media_arr, json_type_array) ||
       json_object_array_length(media_arr) == 0) {
      return NULL;
   }

   json_object *media = json_object_array_get_idx(media_arr, 0);
   json_object *part_arr;
   if (!json_object_object_get_ex(media, "Part", &part_arr) ||
       !json_object_is_type(part_arr, json_type_array) || json_object_array_length(part_arr) == 0) {
      return NULL;
   }

   json_object *part = json_object_array_get_idx(part_arr, 0);
   return json_get_string(part, "key");
}

/** Build a track JSON object from a Plex Metadata item */
static json_object *build_track_json(json_object *item) {
   json_object *track = json_object_new_object();

   json_object_object_add(track, "title", json_object_new_string(json_get_string(item, "title")));
   json_object_object_add(track, "artist",
                          json_object_new_string(json_get_string(item, "grandparentTitle")));
   json_object_object_add(track, "album",
                          json_object_new_string(json_get_string(item, "parentTitle")));

   /* Duration: Plex uses milliseconds, we use seconds */
   int duration_ms = json_get_int(item, "duration");
   json_object_object_add(track, "duration_sec", json_object_new_int(duration_ms / 1000));

   /* Path: plex:{Part.key} — no token! */
   const char *part_key = get_part_key(item);
   if (part_key && part_key[0]) {
      char plex_path[1024];
      snprintf(plex_path, sizeof(plex_path), "plex:%s", part_key);
      json_object_object_add(track, "path", json_object_new_string(plex_path));
   }

   /* Store ratingKey for scrobble */
   json_object_object_add(track, "rating_key",
                          json_object_new_string(json_get_string(item, "ratingKey")));

   /* Store updatedAt timestamp (used as mtime for change detection in sync) */
   json_object_object_add(track, "updated_at",
                          json_object_new_int(json_get_int(item, "updatedAt")));

   /* Extract genres as comma-separated string */
   json_object *genre_arr;
   if (json_object_object_get_ex(item, "Genre", &genre_arr) &&
       json_object_is_type(genre_arr, json_type_array)) {
      char genre_buf[256] = "";
      int glen = 0;
      int gcount = (int)json_object_array_length(genre_arr);
      for (int g = 0; g < gcount && glen < (int)sizeof(genre_buf) - 2; g++) {
         json_object *genre_obj = json_object_array_get_idx(genre_arr, g);
         const char *tag = json_get_string(genre_obj, "tag");
         if (tag[0]) {
            if (glen > 0)
               genre_buf[glen++] = ',';
            int written = snprintf(genre_buf + glen, sizeof(genre_buf) - glen, "%s", tag);
            int remaining = (int)(sizeof(genre_buf) - glen);
            glen += (written < remaining) ? written : (remaining - 1);
         }
      }
      if (glen > 0) {
         json_object_object_add(track, "genre", json_object_new_string(genre_buf));
      }
   }

   return track;
}

/**
 * Ensure the music section ID is known.
 * Uses cached value if available, otherwise discovers it.
 * Protected by s_api_mutex to prevent race conditions.
 */
static int ensure_section_id(void) {
   pthread_mutex_lock(&s_api_mutex);

   if (s_section_id > 0) {
      pthread_mutex_unlock(&s_api_mutex);
      return SUCCESS;
   }

   /* Check config override first */
   if (g_config.music.plex.music_section_id > 0) {
      s_section_id = g_config.music.plex.music_section_id;
      pthread_mutex_unlock(&s_api_mutex);
      return SUCCESS;
   }

   pthread_mutex_unlock(&s_api_mutex);

   /* Discovery calls api_get() which takes s_api_mutex internally.
    * plex_client_discover_section writes to s_section_id — at worst
    * two threads discover concurrently and write the same value. */
   return plex_client_discover_section(&s_section_id);
}

/* =============================================================================
 * Public API: Lifecycle
 * ============================================================================= */

int plex_client_init(void) {
   /* Idempotent — safe to call from multiple init paths */
   if (s_api_curl) {
      return SUCCESS;
   }

   s_api_curl = curl_easy_init();
   if (!s_api_curl) {
      OLOG_ERROR("Plex: failed to create API CURL handle");
      return FAILURE;
   }

   s_download_curl = curl_easy_init();
   if (!s_download_curl) {
      OLOG_ERROR("Plex: failed to create download CURL handle");
      curl_easy_cleanup(s_api_curl);
      s_api_curl = NULL;
      return FAILURE;
   }

   /* Generate client_identifier if not set */
   dawn_config_t *mutable_config = (dawn_config_t *)config_get();
   if (!mutable_config->music.plex.client_identifier[0]) {
      uuid_t uuid;
      uuid_generate(uuid);
      uuid_unparse_lower(uuid, mutable_config->music.plex.client_identifier);
      OLOG_INFO("Plex: generated client identifier: %s",
                mutable_config->music.plex.client_identifier);
   }

   /* Clean up orphaned temp files from previous runs */
   plex_client_cleanup_temp_files();

   s_section_id = 0; /* Reset cached section */

   OLOG_INFO("Plex client initialized");
   return SUCCESS;
}

void plex_client_cleanup(void) {
   if (s_api_curl) {
      curl_easy_cleanup(s_api_curl);
      s_api_curl = NULL;
   }
   if (s_download_curl) {
      curl_easy_cleanup(s_download_curl);
      s_download_curl = NULL;
   }
   invalidate_api_headers();
   s_section_id = 0;

   OLOG_INFO("Plex client cleaned up");
}

bool plex_client_is_configured(void) {
   return (g_config.music.plex.host[0] != '\0' && g_secrets.plex_token[0] != '\0');
}

/* =============================================================================
 * Public API: Section Discovery
 * ============================================================================= */

int plex_client_discover_section(int *section_id_out) {
   if (!section_id_out)
      return FAILURE;

   json_object *root = api_get("/library/sections");
   if (!root)
      return FAILURE;

   /* Navigate: MediaContainer.Directory[] */
   json_object *container;
   if (!json_object_object_get_ex(root, "MediaContainer", &container)) {
      OLOG_ERROR("Plex: no MediaContainer in /library/sections response");
      json_object_put(root);
      return FAILURE;
   }

   json_object *dirs;
   if (!json_object_object_get_ex(container, "Directory", &dirs) ||
       !json_object_is_type(dirs, json_type_array)) {
      OLOG_ERROR("Plex: no Directory array in /library/sections");
      json_object_put(root);
      return FAILURE;
   }

   int count = (int)json_object_array_length(dirs);
   for (int i = 0; i < count; i++) {
      json_object *dir = json_object_array_get_idx(dirs, i);
      const char *type = json_get_string(dir, "type");
      if (strcmp(type, "artist") == 0) {
         int key = atoi(json_get_string(dir, "key"));
         if (key > 0) {
            *section_id_out = key;
            const char *title = json_get_string(dir, "title");
            OLOG_INFO("Plex: discovered music section '%s' (id=%d)", title, key);
            json_object_put(root);
            return SUCCESS;
         }
      }
   }

   OLOG_ERROR("Plex: no music library section found");
   json_object_put(root);
   return FAILURE;
}

/* =============================================================================
 * Public API: Library Browsing
 * ============================================================================= */

json_object *plex_client_list_all_tracks(int offset, int limit) {
   if (ensure_section_id() != SUCCESS)
      return NULL;

   char endpoint[256];
   snprintf(endpoint, sizeof(endpoint),
            "/library/sections/%d/all?type=10&X-Plex-Container-Start=%d"
            "&X-Plex-Container-Size=%d",
            s_section_id, offset, limit);

   json_object *root = api_get(endpoint);
   if (!root)
      return NULL;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "browse_type", json_object_new_string("tracks"));

   json_object *container;
   json_object_object_get_ex(root, "MediaContainer", &container);

   int total = container ? json_get_int(container, "totalSize") : 0;
   json_object_object_add(response, "total_count", json_object_new_int(total));
   json_object_object_add(response, "offset", json_object_new_int(offset));
   json_object_object_add(response, "limit", json_object_new_int(limit));

   json_object *tracks_arr = json_object_new_array();
   json_object *metadata;
   if (container && json_object_object_get_ex(container, "Metadata", &metadata) &&
       json_object_is_type(metadata, json_type_array)) {
      int count = (int)json_object_array_length(metadata);
      for (int i = 0; i < count; i++) {
         json_object *item = json_object_array_get_idx(metadata, i);
         json_object *track = build_track_json(item);
         json_object_array_add(tracks_arr, track);
      }
   }

   json_object_object_add(response, "tracks", tracks_arr);
   json_object_put(root);
   return response;
}

/* =============================================================================
 * Public API: Streaming (Download)
 * ============================================================================= */

int plex_client_download_track(const char *part_key, char *out_path, size_t out_path_size) {
   if (!part_key || !out_path || out_path_size == 0)
      return FAILURE;

   if (!s_download_curl) {
      OLOG_ERROR("Plex: download CURL handle not initialized");
      return FAILURE;
   }

   /* Validate Part.key starts with expected path */
   if (strncmp(part_key, "/library/parts/", 15) != 0 &&
       strncmp(part_key, "/library/metadata/", 18) != 0) {
      OLOG_ERROR("Plex: invalid Part.key: %s", part_key);
      return FAILURE;
   }

   /* Build URL without token — auth is header-only to prevent token
    * appearing in logs, core dumps, or /proc introspection */
   char base_url[512];
   if (build_base_url(base_url, sizeof(base_url)) != SUCCESS)
      return FAILURE;

   char url[2048];
   int n = snprintf(url, sizeof(url), "%s%s", base_url, part_key);
   if (n < 0 || (size_t)n >= sizeof(url)) {
      OLOG_ERROR("Plex: download URL too long");
      return FAILURE;
   }

   /* Extract extension from Part.key for temp file suffix */
   const char *ext = extract_extension(part_key);

   /* Auth via headers only (token in X-Plex-Token header) */
   struct curl_slist *headers = get_api_headers();

   /* Ensure /var/tmp/ parent dir exists for temp files */
   (void)mkdir("/var/tmp", 0755);

   /* Configure SSL and perform download under mutex */
   pthread_mutex_lock(&s_download_mutex);

   curl_easy_reset(s_download_curl);
   const plex_config_t *plex = &g_config.music.plex;
   if (plex->ssl && !plex->ssl_verify) {
      curl_easy_setopt(s_download_curl, CURLOPT_SSL_VERIFYPEER, 0L);
      curl_easy_setopt(s_download_curl, CURLOPT_SSL_VERIFYHOST, 2L);
   }

   int result = http_download_to_temp(s_download_curl, url, headers, ext, PLEX_TEMP_PREFIX,
                                      PLEX_DOWNLOAD_MAX_SIZE, out_path, out_path_size);

   pthread_mutex_unlock(&s_download_mutex);

   /* headers are cached — do not free */

   if (result != 0) {
      OLOG_ERROR("Plex: failed to download track: %s", part_key);
      return FAILURE;
   }

   OLOG_INFO("Plex: downloaded track to %s", out_path);
   return SUCCESS;
}

/* =============================================================================
 * Public API: Playback Reporting
 * ============================================================================= */

int plex_client_scrobble(const char *rating_key) {
   if (!rating_key || !rating_key[0])
      return FAILURE;
   if (!is_valid_rating_key(rating_key)) {
      OLOG_WARNING("Plex: invalid rating_key (non-numeric): %s", rating_key);
      return FAILURE;
   }

   char endpoint[256];
   snprintf(endpoint, sizeof(endpoint), "/:/scrobble?key=%s&identifier=com.plexapp.plugins.library",
            rating_key);

   json_object *root = api_get(endpoint);
   if (!root) {
      /* Scrobble is fire-and-forget, but report the failure */
      OLOG_WARNING("Plex: scrobble failed for key %s", rating_key);
      return FAILURE;
   }
   json_object_put(root);
   return SUCCESS;
}

/* =============================================================================
 * Public API: Library Change Detection
 * ============================================================================= */

int plex_client_get_library_updated_at(time_t *updated_at_out) {
   if (!updated_at_out)
      return FAILURE;

   if (ensure_section_id() != SUCCESS)
      return FAILURE;

   char endpoint[256];
   snprintf(endpoint, sizeof(endpoint), "/library/sections/%d", s_section_id);

   json_object *root = api_get(endpoint);
   if (!root)
      return FAILURE;

   json_object *container;
   if (!json_object_object_get_ex(root, "MediaContainer", &container)) {
      json_object_put(root);
      return FAILURE;
   }

   /* Get scannedAt from the Directory array (first element) */
   json_object *dirs;
   if (json_object_object_get_ex(container, "Directory", &dirs) &&
       json_object_is_type(dirs, json_type_array) && json_object_array_length(dirs) > 0) {
      json_object *dir = json_object_array_get_idx(dirs, 0);
      *updated_at_out = (time_t)json_get_int(dir, "scannedAt");
   } else {
      /* Fallback: try updatedAt on the container itself */
      *updated_at_out = (time_t)json_get_int(container, "updatedAt");
   }

   json_object_put(root);
   return SUCCESS;
}

/* =============================================================================
 * Public API: Connection Testing
 * ============================================================================= */

int plex_client_test_connection(char *server_name_out, size_t name_size) {
   json_object *root = api_get("/identity");
   if (!root)
      return FAILURE;

   json_object *container;
   if (json_object_object_get_ex(root, "MediaContainer", &container)) {
      if (server_name_out && name_size > 0) {
         const char *name = json_get_string(container, "friendlyName");
         safe_strncpy(server_name_out, name, name_size);
      }
      OLOG_INFO("Plex: connected to server '%s'", json_get_string(container, "friendlyName"));
   }

   json_object_put(root);
   return SUCCESS;
}

/* =============================================================================
 * Public API: Temp File Cleanup
 * ============================================================================= */

void plex_client_cleanup_temp_files(void) {
   glob_t globbuf;
   char pattern[128];
   snprintf(pattern, sizeof(pattern), "%s*", PLEX_TEMP_PREFIX);

   if (glob(pattern, GLOB_NOSORT, NULL, &globbuf) == 0) {
      for (size_t i = 0; i < globbuf.gl_pathc; i++) {
         if (unlink(globbuf.gl_pathv[i]) == 0) {
            OLOG_INFO("Plex: cleaned up orphaned temp file: %s", globbuf.gl_pathv[i]);
         }
      }
      globfree(&globbuf);
   }
}
