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
 * Document Index Tool — download and index documents from URLs into RAG database
 *
 * Security: SSRF-safe redirect handling, per-user rate limiting, filename
 * sanitization, curl timeouts, protocol restriction, audit logging.
 */

#include "tools/document_index_tool.h"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/curl_buffer.h"
#include "core/embedding_engine.h"
#include "logging.h"
#include "tools/document_extract.h"
#include "tools/document_index_pipeline.h"
#include "tools/tool_registry.h"
#include "tools/url_fetcher.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define DOC_INDEX_CURL_CONNECT_TIMEOUT 5    /* seconds */
#define DOC_INDEX_CURL_TRANSFER_TIMEOUT 10  /* seconds */
#define DOC_INDEX_CURL_LOW_SPEED_LIMIT 1024 /* bytes/sec minimum */
#define DOC_INDEX_CURL_LOW_SPEED_TIME 10    /* seconds below limit before abort */
#define DOC_INDEX_MAX_REDIRECTS 5
#define DOC_INDEX_MAX_FILENAME 255
#define DOC_INDEX_RATE_LIMIT_MAX 5     /* max calls per window */
#define DOC_INDEX_RATE_LIMIT_WINDOW 60 /* window in seconds */
#define DOC_INDEX_RATE_LIMIT_SLOTS 32  /* max tracked users */

/* =============================================================================
 * Rate Limiter
 * ============================================================================= */

typedef struct {
   int user_id;
   time_t timestamps[DOC_INDEX_RATE_LIMIT_MAX];
   int count;
} rate_limit_entry_t;

static rate_limit_entry_t s_rate_limits[DOC_INDEX_RATE_LIMIT_SLOTS];
static bool s_rate_limits_initialized = false;
static pthread_mutex_t s_rate_mutex = PTHREAD_MUTEX_INITIALIZER;

static void rate_limit_init_once(void) {
   if (!s_rate_limits_initialized) {
      for (int i = 0; i < DOC_INDEX_RATE_LIMIT_SLOTS; i++)
         s_rate_limits[i].user_id = -1;
      s_rate_limits_initialized = true;
   }
}

static bool rate_limit_check(int user_id) {
   pthread_mutex_lock(&s_rate_mutex);
   rate_limit_init_once();
   time_t now = time(NULL);

   /* Find or create entry for this user */
   int slot = -1;
   int oldest_slot = 0;
   time_t oldest_time = now;

   for (int i = 0; i < DOC_INDEX_RATE_LIMIT_SLOTS; i++) {
      if (s_rate_limits[i].user_id == user_id) {
         slot = i;
         break;
      }
      if (s_rate_limits[i].user_id == -1) {
         slot = i;
         break;
      }
      /* Track oldest for eviction */
      if (s_rate_limits[i].count > 0 && s_rate_limits[i].timestamps[0] < oldest_time) {
         oldest_time = s_rate_limits[i].timestamps[0];
         oldest_slot = i;
      }
   }

   if (slot < 0) {
      slot = oldest_slot; /* Evict oldest */
      s_rate_limits[slot].user_id = -1;
      s_rate_limits[slot].count = 0;
   }

   rate_limit_entry_t *entry = &s_rate_limits[slot];
   entry->user_id = user_id;

   /* Expire old timestamps */
   int valid = 0;
   for (int i = 0; i < entry->count; i++) {
      if (now - entry->timestamps[i] < DOC_INDEX_RATE_LIMIT_WINDOW) {
         entry->timestamps[valid++] = entry->timestamps[i];
      }
   }
   entry->count = valid;

   if (entry->count >= DOC_INDEX_RATE_LIMIT_MAX) {
      pthread_mutex_unlock(&s_rate_mutex);
      return false; /* Rate limited */
   }

   /* Record this call */
   entry->timestamps[entry->count++] = now;
   pthread_mutex_unlock(&s_rate_mutex);
   return true; /* Allowed */
}

/* =============================================================================
 * Filename Sanitization
 * ============================================================================= */

static void sanitize_filename(char *filename, size_t max_len) {
   size_t len = strlen(filename);
   if (len > max_len)
      len = max_len;

   size_t out = 0;
   for (size_t i = 0; i < len; i++) {
      char c = filename[i];
      /* Strip path separators, control chars, DEL, HTML-special chars */
      if (c == '/' || c == '\\' || c < 0x20 || c == 0x7F || c == '<' || c == '>' || c == '"' ||
          c == '\'' || c == '&')
         continue;
      filename[out++] = c;
   }
   filename[out] = '\0';

   /* Ensure not empty */
   if (out == 0) {
      snprintf(filename, max_len, "downloaded_document");
   }
}

static void derive_filename_from_url(const char *url, char *filename, size_t filename_size) {
   /* Find last path segment, stripping query string and fragment */
   const char *path_start = strstr(url, "://");
   if (path_start)
      path_start = strchr(path_start + 3, '/');
   if (!path_start) {
      snprintf(filename, filename_size, "downloaded_document");
      return;
   }

   /* Find end of path (before ? or #) */
   const char *path_end = path_start + strlen(path_start);
   const char *query = strchr(path_start, '?');
   const char *fragment = strchr(path_start, '#');
   if (query && query < path_end)
      path_end = query;
   if (fragment && fragment < path_end)
      path_end = fragment;

   /* Find last slash */
   const char *last_slash = path_start;
   for (const char *p = path_start; p < path_end; p++) {
      if (*p == '/')
         last_slash = p;
   }
   if (*last_slash == '/')
      last_slash++;

   size_t seg_len = (size_t)(path_end - last_slash);
   if (seg_len == 0 || seg_len >= filename_size) {
      snprintf(filename, filename_size, "downloaded_document");
      return;
   }

   memcpy(filename, last_slash, seg_len);
   filename[seg_len] = '\0';
   sanitize_filename(filename, filename_size - 1);
}

/* =============================================================================
 * URL Extension Extraction
 * ============================================================================= */

static const char *extract_url_extension(const char *url) {
   /* Find path component (strip query/fragment) */
   const char *path_start = strstr(url, "://");
   if (path_start)
      path_start = strchr(path_start + 3, '/');
   if (!path_start)
      return NULL;

   /* Find end of path */
   const char *path_end = path_start + strlen(path_start);
   const char *query = strchr(path_start, '?');
   const char *fragment = strchr(path_start, '#');
   if (query && query < path_end)
      path_end = query;
   if (fragment && fragment < path_end)
      path_end = fragment;

   /* Find last dot in path */
   const char *last_dot = NULL;
   for (const char *p = path_start; p < path_end; p++) {
      if (*p == '.')
         last_dot = p;
      if (*p == '/')
         last_dot = NULL; /* Reset on directory boundary */
   }

   if (last_dot && last_dot < path_end && (path_end - last_dot) <= 10)
      return last_dot;

   return NULL;
}

/* =============================================================================
 * SSRF-Safe Download with Manual Redirect Handling
 * ============================================================================= */

typedef struct {
   long http_code;
   char content_type[128];
   curl_buffer_t buffer;
} download_result_t;

static char *download_url(const char *url,
                          size_t max_size,
                          download_result_t *result,
                          char *error_buf,
                          size_t error_size,
                          bool *out_was_blocked) {
   char current_url[2048];
   snprintf(current_url, sizeof(current_url), "%s", url);
   if (out_was_blocked)
      *out_was_blocked = false;

   for (int redirect = 0; redirect <= DOC_INDEX_MAX_REDIRECTS; redirect++) {
      /* SSRF check with DNS resolution + pinning to prevent TOCTOU/rebinding */
      char resolved_ip[INET6_ADDRSTRLEN] = "";
      char host[256] = "";
      int port = 0;
      if (url_is_blocked_with_resolve(current_url, resolved_ip, host, sizeof(host), &port)) {
         snprintf(error_buf, error_size, "URL blocked: cannot access private/internal addresses");
         if (out_was_blocked)
            *out_was_blocked = true;
         return NULL;
      }

      CURL *curl = curl_easy_init();
      if (!curl) {
         snprintf(error_buf, error_size, "Failed to initialize HTTP client");
         return NULL;
      }

      /* Pin DNS resolution to prevent rebinding between check and connect */
      struct curl_slist *resolve_list = NULL;
      if (resolved_ip[0] && host[0] && port > 0) {
         char resolve_entry[512];
         snprintf(resolve_entry, sizeof(resolve_entry), "%s:%d:%s", host, port, resolved_ip);
         resolve_list = curl_slist_append(NULL, resolve_entry);
         if (resolve_list)
            curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve_list);
      }

      curl_buffer_init_with_max(&result->buffer, max_size);

      curl_easy_setopt(curl, CURLOPT_URL, current_url);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result->buffer);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L); /* Manual redirect handling */
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)DOC_INDEX_CURL_CONNECT_TIMEOUT);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)DOC_INDEX_CURL_TRANSFER_TIMEOUT);
      curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, (long)DOC_INDEX_CURL_LOW_SPEED_LIMIT);
      curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)DOC_INDEX_CURL_LOW_SPEED_TIME);
      DAWN_CURL_SET_PROTOCOLS(curl, "http,https");
      curl_easy_setopt(curl, CURLOPT_USERAGENT, URL_FETCH_USER_AGENT);

      CURLcode res = curl_easy_perform(curl);
      if (res != CURLE_OK) {
         snprintf(error_buf, error_size, "Download failed: %s", curl_easy_strerror(res));
         curl_slist_free_all(resolve_list);
         curl_easy_cleanup(curl);
         curl_buffer_free(&result->buffer);
         return NULL;
      }

      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result->http_code);

      /* Handle redirects */
      if (result->http_code >= 300 && result->http_code < 400) {
         char *location = NULL;
         curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
         if (!location || strlen(location) == 0) {
            snprintf(error_buf, error_size, "HTTP %ld redirect with no Location header",
                     result->http_code);
            curl_slist_free_all(resolve_list);
            curl_easy_cleanup(curl);
            curl_buffer_free(&result->buffer);
            return NULL;
         }
         snprintf(current_url, sizeof(current_url), "%s", location);
         curl_slist_free_all(resolve_list);
         curl_easy_cleanup(curl);
         curl_buffer_free(&result->buffer);
         continue;
      }

      /* Get Content-Type */
      char *ct = NULL;
      curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
      if (ct) {
         snprintf(result->content_type, sizeof(result->content_type), "%s", ct);
      } else {
         result->content_type[0] = '\0';
      }

      curl_slist_free_all(resolve_list);
      curl_easy_cleanup(curl);

      if (result->http_code != 200) {
         snprintf(error_buf, error_size, "HTTP %ld error", result->http_code);
         curl_buffer_free(&result->buffer);
         return NULL;
      }

      if (!result->buffer.data || result->buffer.size == 0) {
         snprintf(error_buf, error_size, "Downloaded content is empty");
         curl_buffer_free(&result->buffer);
         return NULL;
      }

      return result->buffer.data; /* Success */
   }

   snprintf(error_buf, error_size, "Too many redirects (max %d)", DOC_INDEX_MAX_REDIRECTS);
   return NULL;
}

/* =============================================================================
 * Tool Callback
 * ============================================================================= */

static char *doc_index_callback(const char *action, char *value, int *should_respond);
static bool doc_index_is_available(void);

static const treg_param_t doc_index_params[] = {
   {
       .name = "url",
       .description = "The URL of the document to download and index",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
   {
       .name = "title",
       .description = "Optional friendly name for the document (defaults to URL filename)",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "title",
   },
};

static const tool_metadata_t doc_index_metadata = {
   .name = "document_index",
   .device_string = "document indexer",
   .description = "Download a document from a URL and index it for later search. "
                  "Supports PDF, DOCX, HTML, plain text, Markdown, code files, and more. "
                  "After indexing, use document_search to find content from the indexed "
                  "document. Use this when the user asks you to save, remember, or index "
                  "a web page or document URL for reference.",
   .params = doc_index_params,
   .param_count = 2,
   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_NETWORK,
   .callback = doc_index_callback,
   .is_available = doc_index_is_available,
};

static bool doc_index_is_available(void) {
   return embedding_engine_available();
}

static char *doc_index_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   *should_respond = 1;

   char result_buf[512];

   if (!value || strlen(value) == 0) {
      snprintf(result_buf, sizeof(result_buf), "Error: URL parameter is required.");
      return strdup(result_buf);
   }

   /* Extract URL (strip custom params if present) */
   char url[2048];
   const char *sep = strstr(value, "::");
   if (sep) {
      size_t url_len = (size_t)(sep - value);
      if (url_len >= sizeof(url))
         url_len = sizeof(url) - 1;
      memcpy(url, value, url_len);
      url[url_len] = '\0';
   } else {
      snprintf(url, sizeof(url), "%s", value);
   }

   /* Trim whitespace */
   char *url_start = url;
   while (*url_start == ' ')
      url_start++;
   size_t url_len = strlen(url_start);
   while (url_len > 0 && url_start[url_len - 1] == ' ')
      url_start[--url_len] = '\0';

   /* Extract optional title */
   char title[DOC_INDEX_MAX_FILENAME + 1] = "";
   tool_param_extract_custom(value, "title", title, sizeof(title));

   /* Rate limit check */
   int user_id = tool_get_current_user_id();
   if (!rate_limit_check(user_id)) {
      snprintf(result_buf, sizeof(result_buf),
               "Rate limit: max %d document indexing requests per minute.",
               DOC_INDEX_RATE_LIMIT_MAX);
      return strdup(result_buf);
   }

   /* Validate URL */
   if (!url_is_valid(url_start)) {
      snprintf(result_buf, sizeof(result_buf), "Invalid URL: must start with http:// or https://");
      return strdup(result_buf);
   }

   if (url_is_blocked(url_start)) {
      snprintf(result_buf, sizeof(result_buf),
               "URL blocked: cannot access private/internal addresses.");
      return strdup(result_buf);
   }

   /* Determine expected type from URL path */
   const char *url_ext = extract_url_extension(url_start);
   const char *ext = url_ext ? url_ext : ".html"; /* Default to HTML for extensionless URLs */

   /* Check extension allowed (preliminary — Content-Type may override) */
   if (url_ext && !document_extension_allowed(url_ext)) {
      snprintf(result_buf, sizeof(result_buf), "Unsupported file type: %s", url_ext);
      return strdup(result_buf);
   }

   /* Download with SSRF-safe redirect handling */
   size_t max_download = (size_t)g_config.documents.max_file_size_kb * 1024;
   download_result_t dl = { 0 };
   char dl_error[256] = "";
   bool used_flaresolverr = false;
   char resolved_ext[16] = "";

   doc_extract_result_t extract = { 0 };

   bool was_blocked = false;
   if (!download_url(url_start, max_download, &dl, dl_error, sizeof(dl_error), &was_blocked)) {
      /* Direct download failed — try FlareSolverr fallback for HTML content
       * but NOT if the URL was blocked by SSRF protection */
      bool is_html_type = !was_blocked && (!url_ext || strcasecmp(ext, ".html") == 0 ||
                                           strcasecmp(ext, ".htm") == 0);
      if (is_html_type) {
         char *fs_content = NULL;
         size_t fs_size = 0;
         int fs_rc = url_fetch_content(url_start, &fs_content, &fs_size);
         if (fs_rc == URL_FETCH_SUCCESS && fs_content && fs_size > 0) {
            OLOG_INFO("document_index: FlareSolverr fallback succeeded for [%s]", url_start);
            /* Cap FlareSolverr content at configured max extracted size */
            size_t max_extract = (size_t)g_config.documents.max_extracted_size_kb * 1024;
            if (fs_size > max_extract) {
               fs_content[max_extract] = '\0';
               fs_size = max_extract;
            }
            /* url_fetch_content returns markdown — index as .md */
            ext = ".md";
            extract.text = fs_content;
            extract.text_len = fs_size;
            extract.page_count = -1;
            used_flaresolverr = true;
            /* Skip normal extraction path — text is already clean markdown */
            goto index_document;
         }
         free(fs_content);
      }
      snprintf(result_buf, sizeof(result_buf), "Download failed: %s", dl_error);
      return strdup(result_buf);
   }

   /* Resolve file type from Content-Type (authoritative) */
   if (dl.content_type[0]) {
      const char *ct_ext = document_extension_from_content_type(dl.content_type);
      if (ct_ext) {
         snprintf(resolved_ext, sizeof(resolved_ext), "%s", ct_ext);
         ext = resolved_ext;
      }
      /* If Content-Type is unknown (application/octet-stream), keep URL extension */
   }

   /* Verify resolved extension is allowed */
   if (!document_extension_allowed(ext)) {
      snprintf(result_buf, sizeof(result_buf), "Unsupported content type: %s",
               dl.content_type[0] ? dl.content_type : "(unknown)");
      curl_buffer_free(&dl.buffer);
      return strdup(result_buf);
   }

   /* Extract text */
   int extract_rc = document_extract_from_buffer(
       dl.buffer.data, dl.buffer.size, ext, (size_t)g_config.documents.max_extracted_size_kb * 1024,
       g_config.documents.max_pages, &extract);

   /* Free download buffer immediately to reduce peak memory */
   curl_buffer_free(&dl.buffer);

   if (extract_rc != DOC_EXTRACT_SUCCESS) {
      /* Extraction failed — try FlareSolverr fallback for HTML content */
      bool is_html_ext = strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0;
      if (is_html_ext) {
         char *fs_content = NULL;
         size_t fs_size = 0;
         int fs_rc = url_fetch_content(url_start, &fs_content, &fs_size);
         if (fs_rc == URL_FETCH_SUCCESS && fs_content && fs_size > 0) {
            OLOG_INFO("document_index: FlareSolverr fallback succeeded for [%s]", url_start);
            size_t max_extract = (size_t)g_config.documents.max_extracted_size_kb * 1024;
            if (fs_size > max_extract) {
               fs_content[max_extract] = '\0';
               fs_size = max_extract;
            }
            ext = ".md";
            extract.text = fs_content;
            extract.text_len = fs_size;
            extract.page_count = -1;
            used_flaresolverr = true;
            goto index_document;
         }
         free(fs_content);
      }
      snprintf(result_buf, sizeof(result_buf), "Extraction failed: %s",
               document_extract_error_string(extract_rc));
      return strdup(result_buf);
   }

index_document:
   (void)used_flaresolverr; /* Used in logging below */

   /* Derive filename */
   char filename[DOC_INDEX_MAX_FILENAME + 1];
   if (title[0]) {
      snprintf(filename, sizeof(filename), "%s", title);
      sanitize_filename(filename, DOC_INDEX_MAX_FILENAME);
   } else {
      derive_filename_from_url(url_start, filename, sizeof(filename));
   }

   /* Index (is_global intentionally false — LLM cannot make documents globally visible) */
   doc_index_result_t idx_result;
   int idx_rc = document_index_text(user_id, filename, ext, extract.text, extract.text_len, false,
                                    NULL, &idx_result);

   free(extract.text);

   /* Audit log — strip query params for security */
   {
      char safe_url[512];
      snprintf(safe_url, sizeof(safe_url), "%s", url_start);
      char *qmark = strchr(safe_url, '?');
      if (qmark)
         *qmark = '\0';
      char *hash = strchr(safe_url, '#');
      if (hash)
         *hash = '\0';

      if (idx_rc == DOC_INDEX_SUCCESS) {
         OLOG_INFO("document_index: user %d indexed [%s] as '%s' (%d chunks%s)", user_id, safe_url,
                   filename, idx_result.num_chunks, used_flaresolverr ? ", via FlareSolverr" : "");
      } else {
         OLOG_INFO("document_index: user %d failed to index [%s]: %s", user_id, safe_url,
                   idx_result.error_msg);
      }
   }

   if (idx_rc != DOC_INDEX_SUCCESS) {
      snprintf(result_buf, sizeof(result_buf), "Indexing failed: %s",
               idx_result.error_msg[0] ? idx_result.error_msg
                                       : document_index_error_string(idx_rc));
      return strdup(result_buf);
   }

   snprintf(result_buf, sizeof(result_buf),
            "Successfully indexed '%s' (%d chunks). Use document_search to find content from this "
            "document.",
            filename, idx_result.num_chunks);
   return strdup(result_buf);
}

/* =============================================================================
 * Registration
 * ============================================================================= */

int document_index_tool_register(void) {
   return tool_registry_register(&doc_index_metadata);
}
