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
 * Image Search Tool - Server-side image search via SearXNG
 *
 * Searches for images, fetches them concurrently via curl_multi, caches via
 * the image store, and returns local /api/images/ URLs to the LLM.
 */

#include "tools/image_search_tool.h"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <mosquitto.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/curl_buffer.h"
#include "core/ocp_helpers.h"
#include "core/session_manager.h"
#include "core/worker_pool.h"
#include "image_store.h"
#include "logging.h"
#include "tools/tool_registry.h"
#include "tools/url_fetcher.h"
#include "tools/web_search.h"

/* ========== Constants ========== */

#define IMAGE_SEARCH_MAX_COUNT 10
#define IMAGE_SEARCH_DEFAULT_COUNT 5
#define IMAGE_SEARCH_WALL_CLOCK_SEC 10
#define IMAGE_SEARCH_PER_TRANSFER_SEC 10
#define IMAGE_SEARCH_MAX_FETCH_SIZE \
   (1 * 1024 * 1024) /* 1MB per image — web results rarely larger */
#define IMAGE_SEARCH_RESULT_BUF_SIZE 4096

/* ========== Forward Declarations ========== */

static char *image_search_callback(const char *action, char *value, int *should_respond);
static bool image_search_is_available(void);

/* ========== Magic Byte Validation ========== */

typedef struct {
   const unsigned char *magic;
   size_t magic_len;
   const char *mime_type;
} magic_entry_t;

static const unsigned char MAGIC_JPEG[] = { 0xFF, 0xD8, 0xFF };
static const unsigned char MAGIC_PNG[] = { 0x89, 0x50, 0x4E, 0x47 };
static const unsigned char MAGIC_GIF87[] = { 0x47, 0x49, 0x46, 0x38, 0x37, 0x61 }; /* GIF87a */
static const unsigned char MAGIC_GIF89[] = { 0x47, 0x49, 0x46, 0x38, 0x39, 0x61 }; /* GIF89a */
static const unsigned char MAGIC_WEBP[] = { 0x52, 0x49, 0x46, 0x46 };

static const magic_entry_t MAGIC_TABLE[] = {
   { MAGIC_JPEG, 3, "image/jpeg" }, { MAGIC_PNG, 4, "image/png" },
   { MAGIC_GIF87, 6, "image/gif" }, { MAGIC_GIF89, 6, "image/gif" },
   { MAGIC_WEBP, 4, "image/webp" },
};
#define MAGIC_TABLE_COUNT 5

/**
 * @brief Detect MIME type from magic bytes
 * @return MIME type string or NULL if unrecognized
 */
static const char *detect_mime_from_magic(const void *data, size_t size) {
   if (!data || size < 4)
      return NULL;

   const unsigned char *bytes = (const unsigned char *)data;
   for (int i = 0; i < MAGIC_TABLE_COUNT; i++) {
      if (size >= MAGIC_TABLE[i].magic_len &&
          memcmp(bytes, MAGIC_TABLE[i].magic, MAGIC_TABLE[i].magic_len) == 0) {
         /* WebP needs additional check: bytes 8-11 must be "WEBP" */
         if (MAGIC_TABLE[i].magic == MAGIC_WEBP) {
            if (size >= 12 && memcmp(bytes + 8, "WEBP", 4) == 0)
               return MAGIC_TABLE[i].mime_type;
            continue;
         }
         return MAGIC_TABLE[i].mime_type;
      }
   }
   return NULL;
}

/* ========== Tool Parameter Definition ========== */

static const treg_param_t image_search_params[] = {
   {
       .name = "query",
       .description = "Image search query",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
   {
       .name = "count",
       .description = "Number of images to fetch and return (1-10, default 5)",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "count",
   },
};

/* ========== Tool Metadata ========== */

static const tool_metadata_t image_search_metadata = {
   .name = "image_search",
   .device_string = "image_search",
   .topic = "dawn",
   .aliases = { NULL },
   .alias_count = 0,

   .description =
       "Search the web for images. Returns an array of locally-cached image URLs. "
       "IMPORTANT: Display results using markdown image syntax: ![title](url) for each image. "
       "Routing by session type: "
       "WebUI session → count=5, display as image grid using markdown images. "
       "HUD/MIRAGE session → count=1 (single focal image for helmet display). "
       "Voice-only satellite → do NOT call this tool; describe the subject verbally instead. "
       "Use specific, descriptive queries for best results.",
   .params = image_search_params,
   .param_count = 2,

   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_NETWORK,
   .skip_followup = false,
   .default_remote = true,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .is_available = image_search_is_available,

   .init = NULL,
   .cleanup = NULL,
   .callback = image_search_callback,
};

/* ========== Availability Check ========== */

static bool image_search_is_available(void) {
   /* Don't gate on web_search_is_initialized() — callback does lazy init (same as search_tool) */
   return g_config.search.endpoint[0] != '\0' && image_store_is_ready();
}

/* ========== Per-Transfer State for curl_multi ========== */

typedef struct {
   curl_buffer_t buffer;
   char img_src[1024];
   struct curl_slist *resolve_list; /* DNS pinning for SSRF prevention */
   char title[256];
   char source[256];
   int width;
   int height;
   bool valid; /* Set false if SSRF blocked or other pre-flight failure */
} image_fetch_t;

/* ========== Callback Implementation ========== */

static char *image_search_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   *should_respond = 1;

   if (!image_search_is_available()) {
      return strdup("Image search is not available (SearXNG or image store not initialized).");
   }

   /* Extract query and count */
   char query[512];
   tool_param_extract_base(value, query, sizeof(query));

   char count_str[16] = "";
   tool_param_extract_custom(value, "count", count_str, sizeof(count_str));
   int count = IMAGE_SEARCH_DEFAULT_COUNT;
   if (count_str[0]) {
      count = atoi(count_str);
   }
   if (count < 1)
      count = 1;
   if (count > IMAGE_SEARCH_MAX_COUNT)
      count = IMAGE_SEARCH_MAX_COUNT;

   OLOG_INFO("image_search: query='%s' count=%d", query, count);

   /* Lazy-init web search module (same pattern as search_tool.c) */
   if (!web_search_is_initialized()) {
      const char *endpoint = g_config.search.endpoint[0] != '\0' ? g_config.search.endpoint : NULL;
      if (web_search_init(endpoint) != 0) {
         return strdup("Image search service is not available.");
      }
   }

   /* Query SearXNG for image results */
   struct json_object *root = web_search_query_images_raw(query, count * 2);
   if (!root) {
      return strdup("Image search request failed.");
   }

   struct json_object *results_array = NULL;
   if (!json_object_object_get_ex(root, "results", &results_array)) {
      OLOG_WARNING("image_search: No 'results' field in SearXNG response");
      json_object_put(root);
      return strdup("[]");
   }

   int result_count = json_object_array_length(results_array);
   if (result_count == 0) {
      json_object_put(root);
      return strdup("[]");
   }

   /* Parse results and prepare fetch list */
   int fetch_count = 0;
   image_fetch_t *fetches = calloc((size_t)count, sizeof(image_fetch_t));
   if (!fetches) {
      json_object_put(root);
      return strdup("Memory allocation failed.");
   }

   for (int i = 0; i < result_count && fetch_count < count; i++) {
      struct json_object *item = json_object_array_get_idx(results_array, (size_t)i);
      if (!item)
         continue;

      struct json_object *val = NULL;

      /* Get img_src (required) */
      if (!json_object_object_get_ex(item, "img_src", &val) || !json_object_get_string(val))
         continue;
      const char *img_src = json_object_get_string(val);
      if (strlen(img_src) >= sizeof(fetches[0].img_src))
         continue;

      /* SSRF check before adding to fetch list */
      char resolved_ip[INET6_ADDRSTRLEN] = "";
      char host[256] = "";
      int port = 0;
      if (url_is_blocked_with_resolve(img_src, resolved_ip, host, sizeof(host), &port)) {
         OLOG_INFO("image_search: SSRF blocked: %s", img_src);
         continue;
      }

      /* Fail-closed: if DNS resolution returned no IP, skip (prevents TOCTOU) */
      if (resolved_ip[0] == '\0') {
         continue;
      }

      image_fetch_t *f = &fetches[fetch_count];
      curl_buffer_init_with_max(&f->buffer, IMAGE_SEARCH_MAX_FETCH_SIZE);
      strncpy(f->img_src, img_src, sizeof(f->img_src) - 1);
      f->valid = true;

      /* Build DNS pinning resolve list to prevent TOCTOU rebinding */
      char resolve_entry[512];
      snprintf(resolve_entry, sizeof(resolve_entry), "%s:%d:%s", host, port ? port : 443,
               resolved_ip);
      f->resolve_list = curl_slist_append(NULL, resolve_entry);
      /* Also pin for port 80 if not already */
      if (port != 80) {
         snprintf(resolve_entry, sizeof(resolve_entry), "%s:80:%s", host, resolved_ip);
         f->resolve_list = curl_slist_append(f->resolve_list, resolve_entry);
      }

      /* Optional fields */
      if (json_object_object_get_ex(item, "title", &val) && json_object_get_string(val)) {
         strncpy(f->title, json_object_get_string(val), sizeof(f->title) - 1);
      }
      if (json_object_object_get_ex(item, "source", &val) && json_object_get_string(val)) {
         strncpy(f->source, json_object_get_string(val), sizeof(f->source) - 1);
      }
      if (json_object_object_get_ex(item, "resolution", &val) && json_object_get_string(val)) {
         /* Resolution is "WxH" string */
         const char *res_str = json_object_get_string(val);
         if (sscanf(res_str, "%dx%d", &f->width, &f->height) != 2) {
            f->width = 0;
            f->height = 0;
         }
      }

      fetch_count++;
   }

   json_object_put(root);

   OLOG_INFO("image_search: %d URLs passed SSRF check (from %d SearXNG results)", fetch_count,
             result_count);

   if (fetch_count == 0) {
      free(fetches);
      return strdup("[]");
   }

   /* Concurrent image fetching via curl_multi (limit parallel connections for Jetson) */
   CURLM *multi = curl_multi_init();
   if (!multi) {
      for (int i = 0; i < fetch_count; i++)
         curl_buffer_free(&fetches[i].buffer);
      free(fetches);
      return strdup("Failed to initialize HTTP client.");
   }

   curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, 4L);

   CURL **easy_handles = calloc((size_t)fetch_count, sizeof(CURL *));
   if (!easy_handles) {
      curl_multi_cleanup(multi);
      for (int i = 0; i < fetch_count; i++)
         curl_buffer_free(&fetches[i].buffer);
      free(fetches);
      return strdup("Memory allocation failed.");
   }

   for (int i = 0; i < fetch_count; i++) {
      CURL *easy = curl_easy_init();
      if (!easy)
         continue;

      easy_handles[i] = easy;
      curl_easy_setopt(easy, CURLOPT_URL, fetches[i].img_src);
      curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
      curl_easy_setopt(easy, CURLOPT_WRITEDATA, &fetches[i].buffer);
      curl_easy_setopt(easy, CURLOPT_TIMEOUT, (long)IMAGE_SEARCH_PER_TRANSFER_SEC);
      curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 5L);
      curl_easy_setopt(easy, CURLOPT_MAXFILESIZE, (long)IMAGE_SEARCH_MAX_FETCH_SIZE);
      curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L); /* No redirects — prevents SSRF bypass */
      curl_easy_setopt(easy, CURLOPT_USERAGENT, "Mozilla/5.0 (compatible; DAWN Image Fetcher)");
      DAWN_CURL_SET_PROTOCOLS(easy, "http,https");
      curl_easy_setopt(easy, CURLOPT_PRIVATE, &fetches[i]);
      /* Pin DNS to prevent TOCTOU rebinding between SSRF check and connect */
      if (fetches[i].resolve_list) {
         curl_easy_setopt(easy, CURLOPT_RESOLVE, fetches[i].resolve_list);
      }

      curl_multi_add_handle(multi, easy);
   }

   /* Run transfers with wall-clock cap (CLOCK_MONOTONIC — immune to NTP jumps) */
   struct timespec deadline_ts;
   clock_gettime(CLOCK_MONOTONIC, &deadline_ts);
   deadline_ts.tv_sec += IMAGE_SEARCH_WALL_CLOCK_SEC;
   int still_running = 0;

   do {
      curl_multi_perform(multi, &still_running);
      if (still_running) {
         struct timespec now_ts;
         clock_gettime(CLOCK_MONOTONIC, &now_ts);
         if (now_ts.tv_sec > deadline_ts.tv_sec ||
             (now_ts.tv_sec == deadline_ts.tv_sec && now_ts.tv_nsec >= deadline_ts.tv_nsec)) {
            break;
         }
         curl_multi_wait(multi, NULL, 0, 100, NULL);
      }
   } while (still_running > 0);

   /* Final perform + drain to catch late completions */
   curl_multi_perform(multi, &still_running);

   /* Collect redirect targets for second pass (max 1 redirect per image) */
   typedef struct {
      int fetch_idx;
      char url[1024];
   } redirect_t;
   redirect_t redirects[IMAGE_SEARCH_MAX_COUNT];
   int redirect_count = 0;

   int msgs_left = 0;
   CURLMsg *msg;
   while ((msg = curl_multi_info_read(multi, &msgs_left))) {
      if (msg->msg == CURLMSG_DONE) {
         image_fetch_t *f = NULL;
         curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &f);
         if (f && msg->data.result != CURLE_OK) {
            f->valid = false;
            continue;
         }
         if (!f)
            continue;

         long http_code = 0;
         curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &http_code);

         /* Collect 3xx redirects for second pass with SSRF re-check */
         if (http_code >= 300 && http_code < 400 && redirect_count < IMAGE_SEARCH_MAX_COUNT) {
            char *redir_url = NULL;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_REDIRECT_URL, &redir_url);
            /* Validate scheme before processing redirect */
            if (redir_url && strncmp(redir_url, "http://", 7) != 0 &&
                strncmp(redir_url, "https://", 8) != 0) {
               OLOG_INFO("image_search: Rejected non-HTTP redirect: %s", redir_url);
               f->valid = false;
            } else if (redir_url && strlen(redir_url) >= sizeof(redirects[0].url)) {
               OLOG_INFO("image_search: Redirect URL too long (%zu bytes), skipping",
                         strlen(redir_url));
               f->valid = false;
            } else if (redir_url) {
               /* Find the fetch index for this handle */
               int idx = -1;
               for (int i = 0; i < fetch_count; i++) {
                  if (&fetches[i] == f) {
                     idx = i;
                     break;
                  }
               }
               if (idx >= 0) {
                  redirects[redirect_count].fetch_idx = idx;
                  strncpy(redirects[redirect_count].url, redir_url, sizeof(redirects[0].url) - 1);
                  redirects[redirect_count].url[sizeof(redirects[0].url) - 1] = '\0';
                  redirect_count++;
                  f->valid = false; /* Mark as needing retry */
                  OLOG_INFO("image_search: %ld redirect for %s -> %s", http_code, f->img_src,
                            redir_url);
               }
            } else {
               f->valid = false;
            }
         } else if (http_code != 200) {
            f->valid = false;
         }
      }
   }

   /* Mark any still-running transfers as invalid (partial data) */
   if (still_running > 0) {
      for (int i = 0; i < fetch_count; i++) {
         if (!easy_handles[i])
            continue;
         long http_code = 0;
         CURLcode info_rc = curl_easy_getinfo(easy_handles[i], CURLINFO_RESPONSE_CODE, &http_code);
         if (info_rc != CURLE_OK || http_code == 0) {
            image_fetch_t *f = NULL;
            curl_easy_getinfo(easy_handles[i], CURLINFO_PRIVATE, &f);
            if (f)
               f->valid = false;
         }
      }
   }

   /* Clean up first-pass curl handles */
   for (int i = 0; i < fetch_count; i++) {
      if (easy_handles[i]) {
         curl_multi_remove_handle(multi, easy_handles[i]);
         curl_easy_cleanup(easy_handles[i]);
         easy_handles[i] = NULL;
      }
      if (fetches[i].resolve_list) {
         curl_slist_free_all(fetches[i].resolve_list);
         fetches[i].resolve_list = NULL;
      }
   }

   /* Second pass: follow redirects with SSRF re-validation */
   if (redirect_count > 0) {
      struct timespec now_ts;
      clock_gettime(CLOCK_MONOTONIC, &now_ts);
      bool time_remaining = now_ts.tv_sec < deadline_ts.tv_sec ||
                            (now_ts.tv_sec == deadline_ts.tv_sec &&
                             now_ts.tv_nsec < deadline_ts.tv_nsec);

      if (time_remaining) {
         OLOG_INFO("image_search: Following %d redirects with SSRF re-check", redirect_count);

         int redir_added = 0;
         for (int r = 0; r < redirect_count; r++) {
            int idx = redirects[r].fetch_idx;
            image_fetch_t *f = &fetches[idx];

            /* SSRF-check the redirect target */
            char resolved_ip[INET6_ADDRSTRLEN] = "";
            char host[256] = "";
            int port = 0;
            if (url_is_blocked_with_resolve(redirects[r].url, resolved_ip, host, sizeof(host),
                                            &port)) {
               OLOG_INFO("image_search: Redirect SSRF blocked: %s", redirects[r].url);
               continue;
            }

            /* Fail-closed: if DNS resolution returned no IP, skip (prevents TOCTOU) */
            if (resolved_ip[0] == '\0') {
               OLOG_INFO("image_search: Redirect DNS failed, skipping: %s", redirects[r].url);
               continue;
            }

            /* Reset buffer for retry */
            curl_buffer_reset(&f->buffer);
            f->valid = true;

            /* Build DNS pinning for redirect target */
            char resolve_entry[512];
            snprintf(resolve_entry, sizeof(resolve_entry), "%s:%d:%s", host, port ? port : 443,
                     resolved_ip);
            f->resolve_list = curl_slist_append(NULL, resolve_entry);
            if (port != 80) {
               snprintf(resolve_entry, sizeof(resolve_entry), "%s:80:%s", host, resolved_ip);
               f->resolve_list = curl_slist_append(f->resolve_list, resolve_entry);
            }

            CURL *easy = curl_easy_init();
            if (!easy) {
               f->valid = false;
               continue;
            }

            easy_handles[idx] = easy;
            curl_easy_setopt(easy, CURLOPT_URL, redirects[r].url);
            curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
            curl_easy_setopt(easy, CURLOPT_WRITEDATA, &f->buffer);
            curl_easy_setopt(easy, CURLOPT_TIMEOUT, (long)IMAGE_SEARCH_PER_TRANSFER_SEC);
            curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 5L);
            curl_easy_setopt(easy, CURLOPT_MAXFILESIZE, (long)IMAGE_SEARCH_MAX_FETCH_SIZE);
            curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
            curl_easy_setopt(easy, CURLOPT_USERAGENT,
                             "Mozilla/5.0 (compatible; DAWN Image Fetcher)");
            DAWN_CURL_SET_PROTOCOLS(easy, "http,https");
            curl_easy_setopt(easy, CURLOPT_PRIVATE, f);
            if (f->resolve_list) {
               curl_easy_setopt(easy, CURLOPT_RESOLVE, f->resolve_list);
            }

            curl_multi_add_handle(multi, easy);
            redir_added++;
         }

         /* Run redirect transfers with remaining wall-clock time */
         if (redir_added > 0) {
            do {
               curl_multi_perform(multi, &still_running);
               if (still_running) {
                  clock_gettime(CLOCK_MONOTONIC, &now_ts);
                  if (now_ts.tv_sec > deadline_ts.tv_sec ||
                      (now_ts.tv_sec == deadline_ts.tv_sec &&
                       now_ts.tv_nsec >= deadline_ts.tv_nsec)) {
                     break;
                  }
                  curl_multi_wait(multi, NULL, 0, 100, NULL);
               }
            } while (still_running > 0);

            curl_multi_perform(multi, &still_running);

            /* Drain redirect results */
            while ((msg = curl_multi_info_read(multi, &msgs_left))) {
               if (msg->msg == CURLMSG_DONE) {
                  image_fetch_t *f = NULL;
                  curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &f);
                  if (f && msg->data.result != CURLE_OK) {
                     f->valid = false;
                  }
                  if (f && f->valid) {
                     long http_code = 0;
                     curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &http_code);
                     if (http_code != 200) {
                        f->valid = false;
                     }
                  }
               }
            }
         }
      }
   }

   /* Clean up all curl handles and resolve lists */
   for (int i = 0; i < fetch_count; i++) {
      if (easy_handles[i]) {
         curl_multi_remove_handle(multi, easy_handles[i]);
         curl_easy_cleanup(easy_handles[i]);
      }
      if (fetches[i].resolve_list) {
         curl_slist_free_all(fetches[i].resolve_list);
         fetches[i].resolve_list = NULL;
      }
   }
   curl_multi_cleanup(multi);
   free(easy_handles);

   /* Validate and cache successfully fetched images */
   struct json_object *result_array = json_object_new_array();
   if (!result_array) {
      for (int i = 0; i < fetch_count; i++)
         curl_buffer_free(&fetches[i].buffer);
      free(fetches);
      return strdup("[]");
   }

   int cached_count = 0;
   for (int i = 0; i < fetch_count; i++) {
      image_fetch_t *f = &fetches[i];
      if (!f->valid || !f->buffer.data || f->buffer.size == 0) {
         curl_buffer_free(&f->buffer);
         continue;
      }

      /* Validate magic bytes */
      const char *mime = detect_mime_from_magic(f->buffer.data, f->buffer.size);
      if (!mime) {
         OLOG_INFO("image_search: Skipping %s (unrecognized format)", f->img_src);
         curl_buffer_free(&f->buffer);
         continue;
      }

      /* Cache via image store (use current session's user_id for FK constraint) */
      char image_id[IMAGE_ID_LEN];
      size_t fetched_size = f->buffer.size;
      int uid = tool_get_current_user_id();
      if (uid <= 0) {
         OLOG_WARNING("image_search: No valid user context, skipping cache for %s", f->img_src);
         curl_buffer_free(&f->buffer);
         continue;
      }
      int rc = image_store_save_ex(uid, f->buffer.data, f->buffer.size, mime, IMAGE_SOURCE_SEARCH,
                                   IMAGE_RETAIN_CACHE, image_id);
      curl_buffer_free(&f->buffer);

      if (rc != IMAGE_STORE_SUCCESS) {
         OLOG_WARNING("image_search: Failed to cache image from %s (rc=%d)", f->img_src, rc);
         continue;
      }

      /* Build result object */
      struct json_object *obj = json_object_new_object();
      if (!obj)
         continue;

      char url[64];
      snprintf(url, sizeof(url), "/api/images/%s", image_id);
      json_object_object_add(obj, "url", json_object_new_string(url));
      if (f->width > 0 && f->height > 0) {
         json_object_object_add(obj, "width", json_object_new_int(f->width));
         json_object_object_add(obj, "height", json_object_new_int(f->height));
      }
      if (f->source[0]) {
         json_object_object_add(obj, "source", json_object_new_string(f->source));
      }
      if (f->title[0]) {
         json_object_object_add(obj, "title", json_object_new_string(f->title));
      }

      json_object_array_add(result_array, obj);
      cached_count++;

      OLOG_INFO("image_search: Cached %s (%zu bytes, %s) as %s", f->img_src, fetched_size, mime,
                image_id);
   }

   free(fetches);

   OLOG_INFO("image_search: Returned %d/%d images for '%s'", cached_count, fetch_count, query);

   /* Publish first result to HUD for local voice sessions (Iron Man suit display) */
   if (cached_count > 0) {
      session_t *session = session_get_command_context();
      if (session && session->type == SESSION_TYPE_LOCAL) {
         struct json_object *first = json_object_array_get_idx(result_array, 0);
         if (first) {
            const char *img_url = NULL;
            const char *img_title = "";
            const char *img_source = "";
            struct json_object *j_tmp;
            if (json_object_object_get_ex(first, "url", &j_tmp))
               img_url = json_object_get_string(j_tmp);
            if (json_object_object_get_ex(first, "title", &j_tmp))
               img_title = json_object_get_string(j_tmp);
            if (json_object_object_get_ex(first, "source", &j_tmp))
               img_source = json_object_get_string(j_tmp);

            if (img_url) {
               struct mosquitto *mosq = worker_pool_get_mosq();
               if (mosq) {
                  struct json_object *hud = json_object_new_object();
                  json_object_object_add(hud, "device", json_object_new_string("image"));
                  json_object_object_add(hud, "action", json_object_new_string("display"));
                  json_object_object_add(hud, "msg_type", json_object_new_string("request"));
                  json_object_object_add(hud, "image_url", json_object_new_string(img_url));
                  json_object_object_add(hud, "title",
                                         json_object_new_string(img_title ? img_title : ""));
                  json_object_object_add(hud, "source",
                                         json_object_new_string(img_source ? img_source : ""));
                  json_object_object_add(hud, "ttl", json_object_new_int(30));
                  json_object_object_add(hud, "timestamp",
                                         json_object_new_int64(ocp_get_timestamp_ms()));

                  const char *hud_str = json_object_to_json_string(hud);
                  mosquitto_publish(mosq, NULL, "hud", (int)strlen(hud_str), hud_str, 0, false);
                  json_object_put(hud);
                  OLOG_INFO("image_search: Published first result to HUD for local session");
               }
            }
         }
      }
   }

   const char *json_str = json_object_to_json_string_ext(result_array, JSON_C_TO_STRING_PLAIN);
   char *result = strdup(json_str ? json_str : "[]");
   json_object_put(result_array);

   return result;
}

/* ========== Registration ========== */

int image_search_tool_register(void) {
   return tool_registry_register(&image_search_metadata);
}
