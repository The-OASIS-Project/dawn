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
 * SmartThings Service Implementation
 *
 * Provides OAuth2 authentication, token management, and device control
 * for Samsung SmartThings smart home devices.
 */

#include "tools/smartthings_service.h"

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <pthread.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config/dawn_config.h"
#include "llm/llm_command_parser.h"
#include "logging.h"

/* =============================================================================
 * Constants
 * ============================================================================= */
#define TOKEN_FILE_PATH "/.config/dawn/smartthings_tokens.json"
#define TOKEN_REFRESH_MARGIN_SEC 300 /* Refresh 5 minutes before expiry */
#define API_TIMEOUT_SEC 30
#define MAX_RESPONSE_SIZE (256 * 1024) /* 256 KB max response (sufficient for ST API) */
#define INITIAL_RESPONSE_SIZE 1024     /* 1 KB initial (most responses <500 bytes) */
#define API_MAX_RETRIES 3              /* Retry failed requests up to 3 times */
#define API_RETRY_BASE_MS 500          /* Base delay for exponential backoff */
#define OAUTH_STATE_LEN 32             /* Length of OAuth state parameter (hex) */

/* =============================================================================
 * Internal State
 * ============================================================================= */

/* Internal authentication mode (matches public st_auth_mode_t) */
typedef enum {
   AUTH_NONE = 0, /* Not configured */
   AUTH_PAT,      /* Personal Access Token (no refresh) */
   AUTH_OAUTH2    /* OAuth2 with refresh tokens */
} internal_auth_mode_t;

typedef struct {
   char access_token[512];
   char refresh_token[512];
   int64_t token_expiry; /* Unix timestamp */
} st_tokens_t;

static struct {
   bool initialized;
   internal_auth_mode_t auth_mode;
   st_tokens_t tokens;
   st_device_list_t device_cache;
   pthread_rwlock_t rwlock;
   pthread_mutex_t token_refresh_mutex; /* Separate mutex for token refresh */
   char token_file_path[512];
   CURL *curl_handle;                             /* Reusable CURL handle */
   char pending_oauth_state[OAUTH_STATE_LEN + 1]; /* CSRF protection for OAuth */
} s_state = { 0 };

/* =============================================================================
 * Secure Memory Operations
 * ============================================================================= */

/**
 * @brief Securely zero memory that cannot be optimized away
 */
static void secure_zero(void *ptr, size_t len) {
#if defined(__linux__) || defined(__unix__)
   explicit_bzero(ptr, len);
#else
   volatile unsigned char *p = ptr;
   while (len--) {
      *p++ = 0;
   }
#endif
}

/**
 * @brief Validate device ID is a valid UUID format (prevents URL injection)
 * @return true if valid, false otherwise
 */
static bool is_valid_device_id(const char *id) {
   if (!id)
      return false;
   size_t len = strlen(id);
   if (len == 0 || len > ST_MAX_DEVICE_ID - 1)
      return false;

   /* UUID format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx or similar hex patterns */
   for (size_t i = 0; i < len; i++) {
      char c = id[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') ||
            c == '-')) {
         return false;
      }
   }
   return true;
}

/* =============================================================================
 * CURL Response Buffer
 * ============================================================================= */
typedef struct {
   char *data;
   size_t size;
   size_t capacity;
} response_buffer_t;

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
   response_buffer_t *buf = (response_buffer_t *)userp;

   /* Check for multiplication overflow */
   if (size > 0 && nmemb > SIZE_MAX / size) {
      return 0; /* Overflow would occur */
   }
   size_t realsize = size * nmemb;

   /* Check for addition overflow */
   if (buf->size > SIZE_MAX - realsize) {
      return 0;
   }

   if (buf->size + realsize >= MAX_RESPONSE_SIZE) {
      return 0; /* Abort on oversized response */
   }

   if (buf->size + realsize >= buf->capacity) {
      size_t new_cap = buf->capacity * 2;
      if (new_cap < buf->size + realsize + 1)
         new_cap = buf->size + realsize + 1;
      char *new_data = realloc(buf->data, new_cap);
      if (!new_data)
         return 0;
      buf->data = new_data;
      buf->capacity = new_cap;
   }

   memcpy(buf->data + buf->size, contents, realsize);
   buf->size += realsize;
   buf->data[buf->size] = '\0';

   return realsize;
}

static void response_buffer_init(response_buffer_t *buf) {
   buf->data = malloc(INITIAL_RESPONSE_SIZE);
   buf->size = 0;
   buf->capacity = buf->data ? INITIAL_RESPONSE_SIZE : 0;
   if (buf->data)
      buf->data[0] = '\0';
}

static void response_buffer_free(response_buffer_t *buf) {
   free(buf->data);
   buf->data = NULL;
   buf->size = 0;
   buf->capacity = 0;
}

/* =============================================================================
 * Token File Management
 * ============================================================================= */

/**
 * @brief Get secure token file path using getpwuid (more secure than getenv)
 * @return 0 on success, 1 on failure (path will be empty)
 */
static int get_token_file_path(char *path, size_t size) {
   if (!path || size < 64) {
      return 1;
   }

   /* Use getpwuid for more secure home directory resolution */
   struct passwd *pw = getpwuid(getuid());
   if (pw && pw->pw_dir && pw->pw_dir[0] == '/') {
      snprintf(path, size, "%s%s", pw->pw_dir, TOKEN_FILE_PATH);
      return 0;
   }

   /* Fall back to XDG_CONFIG_HOME if available and absolute */
   const char *xdg = getenv("XDG_CONFIG_HOME");
   if (xdg && xdg[0] == '/') {
      snprintf(path, size, "%s/dawn/smartthings_tokens.json", xdg);
      return 0;
   }

   /* Last resort: HOME environment variable (less secure) */
   const char *home = getenv("HOME");
   if (home && home[0] == '/') {
      snprintf(path, size, "%s%s", home, TOKEN_FILE_PATH);
      return 0;
   }

   /* Refuse to use /tmp - too insecure for tokens */
   LOG_ERROR("SmartThings: Cannot determine secure token storage path");
   path[0] = '\0';
   return 1;
}

static int load_tokens_from_file(void) {
   FILE *fp = fopen(s_state.token_file_path, "r");
   if (!fp)
      return 1;

   fseek(fp, 0, SEEK_END);
   long len = ftell(fp);
   fseek(fp, 0, SEEK_SET);

   if (len <= 0 || len > 8192) {
      fclose(fp);
      return 1;
   }

   char *data = malloc(len + 1);
   if (!data) {
      fclose(fp);
      return 1;
   }

   size_t read = fread(data, 1, len, fp);
   fclose(fp);
   data[read] = '\0';

   json_object *root = json_tokener_parse(data);
   free(data);

   if (!root)
      return 1;

   json_object *obj;
   if (json_object_object_get_ex(root, "access_token", &obj)) {
      strncpy(s_state.tokens.access_token, json_object_get_string(obj),
              sizeof(s_state.tokens.access_token) - 1);
   }
   if (json_object_object_get_ex(root, "refresh_token", &obj)) {
      strncpy(s_state.tokens.refresh_token, json_object_get_string(obj),
              sizeof(s_state.tokens.refresh_token) - 1);
   }
   if (json_object_object_get_ex(root, "token_expiry", &obj)) {
      s_state.tokens.token_expiry = json_object_get_int64(obj);
   }

   json_object_put(root);

   LOG_INFO("SmartThings: Loaded tokens from %s", s_state.token_file_path);
   return 0;
}

static int save_tokens_to_file(void) {
   if (!s_state.token_file_path[0]) {
      LOG_ERROR("SmartThings: No token file path configured");
      return 1;
   }

   /* Create config directory if needed using secure path from token_file_path */
   char dir_path[512];
   strncpy(dir_path, s_state.token_file_path, sizeof(dir_path) - 1);
   dir_path[sizeof(dir_path) - 1] = '\0';

   /* Find last slash and truncate to get directory */
   char *last_slash = strrchr(dir_path, '/');
   if (last_slash) {
      *last_slash = '\0';
      /* Create parent directories with secure permissions */
      char *p = dir_path + 1;
      while (*p) {
         if (*p == '/') {
            *p = '\0';
            mkdir(dir_path, 0700);
            *p = '/';
         }
         p++;
      }
      mkdir(dir_path, 0700);
   }

   /* Write to temp file first (atomic) using O_NOFOLLOW to prevent symlink attacks */
   char tmp_path[520];
   snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", s_state.token_file_path);

   /* Remove any existing temp file first (in case it's a symlink) */
   unlink(tmp_path);

   /* Open with O_NOFOLLOW, O_CREAT, O_EXCL for security */
   int fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
   if (fd < 0) {
      LOG_ERROR("SmartThings: Failed to create token file: %s", strerror(errno));
      return 1;
   }

   json_object *root = json_object_new_object();
   json_object_object_add(root, "access_token",
                          json_object_new_string(s_state.tokens.access_token));
   json_object_object_add(root, "refresh_token",
                          json_object_new_string(s_state.tokens.refresh_token));
   json_object_object_add(root, "token_expiry", json_object_new_int64(s_state.tokens.token_expiry));

   const char *json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);
   size_t len = strlen(json_str);

   ssize_t written = write(fd, json_str, len);
   close(fd);
   json_object_put(root);

   if (written < 0 || (size_t)written != len) {
      LOG_ERROR("SmartThings: Failed to write token file: %s", strerror(errno));
      unlink(tmp_path);
      return 1;
   }

   /* Atomic rename */
   if (rename(tmp_path, s_state.token_file_path) != 0) {
      LOG_ERROR("SmartThings: Failed to save token file: %s", strerror(errno));
      unlink(tmp_path);
      return 1;
   }

   LOG_INFO("SmartThings: Saved tokens to %s", s_state.token_file_path);
   return 0;
}

/* =============================================================================
 * HTTP Request Helpers
 * ============================================================================= */

/**
 * @brief Get or create reusable CURL handle (more efficient than per-request)
 */
static CURL *get_curl_handle(void) {
   if (s_state.curl_handle) {
      curl_easy_reset(s_state.curl_handle);
      return s_state.curl_handle;
   }
   s_state.curl_handle = curl_easy_init();
   return s_state.curl_handle;
}

/**
 * @brief Configure CURL handle with security settings
 */
static void configure_curl_security(CURL *curl) {
   /* Explicitly enable TLS certificate verification */
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

   /* Require TLS 1.2 or higher */
   curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

   /* Standard timeouts */
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, API_TIMEOUT_SEC);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
}

/**
 * @brief Sleep for exponential backoff delay
 */
static void backoff_sleep(int attempt) {
   /* Exponential backoff: 500ms, 1000ms, 2000ms */
   int delay_ms = API_RETRY_BASE_MS * (1 << attempt);
   if (delay_ms > 5000)
      delay_ms = 5000; /* Cap at 5 seconds */
   usleep(delay_ms * 1000);
}

static st_error_t do_api_request(const char *method,
                                 const char *url,
                                 const char *body,
                                 response_buffer_t *response,
                                 long *http_code) {
   CURL *curl = get_curl_handle();
   if (!curl)
      return ST_ERR_MEMORY;

   struct curl_slist *headers = NULL;
   char auth_header[600];
   snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s",
            s_state.tokens.access_token);
   headers = curl_slist_append(headers, auth_header);
   headers = curl_slist_append(headers, "Content-Type: application/json");

   configure_curl_security(curl);
   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

   if (strcmp(method, "POST") == 0) {
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
   } else if (strcmp(method, "PUT") == 0) {
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
   } else if (strcmp(method, "DELETE") == 0) {
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
   }

   /* Retry loop with exponential backoff for transient errors */
   CURLcode res = CURLE_OK;
   for (int attempt = 0; attempt < API_MAX_RETRIES; attempt++) {
      if (attempt > 0) {
         /* Reset response buffer for retry */
         response->size = 0;
         if (response->data)
            response->data[0] = '\0';

         LOG_INFO("SmartThings: Retrying request (attempt %d/%d)", attempt + 1, API_MAX_RETRIES);
         backoff_sleep(attempt - 1);
      }

      res = curl_easy_perform(curl);
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);

      /* Only retry on network errors or 5xx server errors */
      if (res == CURLE_OK && *http_code < 500) {
         break; /* Success or client error - don't retry */
      }

      if (res != CURLE_OK) {
         LOG_WARNING("SmartThings: Request failed: %s", curl_easy_strerror(res));
      } else {
         LOG_WARNING("SmartThings: Server error %ld, will retry", *http_code);
      }
   }

   curl_slist_free_all(headers);
   /* Note: Don't cleanup curl handle - it's reused */

   if (res != CURLE_OK) {
      LOG_ERROR("SmartThings: CURL error after %d attempts: %s", API_MAX_RETRIES,
                curl_easy_strerror(res));
      return ST_ERR_NETWORK;
   }

   if (*http_code == 401) {
      return ST_ERR_TOKEN_EXPIRED;
   } else if (*http_code == 429) {
      return ST_ERR_RATE_LIMITED;
   } else if (*http_code >= 400) {
      /* Don't log response body - may contain sensitive info */
      LOG_ERROR("SmartThings: API error %ld", *http_code);
      return ST_ERR_API;
   }

   return ST_OK;
}

/* =============================================================================
 * Token Refresh
 * ============================================================================= */
static st_error_t refresh_access_token(void) {
   const secrets_config_t *secrets = config_get_secrets();
   if (!secrets || !secrets->smartthings_client_id[0] || !secrets->smartthings_client_secret[0]) {
      return ST_ERR_NOT_CONFIGURED;
   }

   if (!s_state.tokens.refresh_token[0]) {
      return ST_ERR_NOT_AUTHENTICATED;
   }

   LOG_INFO("SmartThings: Refreshing access token...");

   CURL *curl = get_curl_handle();
   if (!curl)
      return ST_ERR_MEMORY;

   response_buffer_t response;
   response_buffer_init(&response);

   /* Build POST body */
   char post_data[2048];
   char *encoded_client_id = curl_easy_escape(curl, secrets->smartthings_client_id, 0);
   char *encoded_client_secret = curl_easy_escape(curl, secrets->smartthings_client_secret, 0);
   char *encoded_refresh = curl_easy_escape(curl, s_state.tokens.refresh_token, 0);

   snprintf(post_data, sizeof(post_data),
            "grant_type=refresh_token&client_id=%s&client_secret=%s&refresh_token=%s",
            encoded_client_id, encoded_client_secret, encoded_refresh);

   curl_free(encoded_client_id);
   curl_free(encoded_client_secret);
   curl_free(encoded_refresh);

   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

   configure_curl_security(curl);
   curl_easy_setopt(curl, CURLOPT_URL, ST_TOKEN_URL);
   curl_easy_setopt(curl, CURLOPT_POST, 1L);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

   CURLcode res = curl_easy_perform(curl);
   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

   curl_slist_free_all(headers);
   /* Note: Don't cleanup curl handle - it's reused */

   /* Securely clear post_data containing client secret */
   secure_zero(post_data, sizeof(post_data));

   if (res != CURLE_OK) {
      LOG_ERROR("SmartThings: Token refresh failed: %s", curl_easy_strerror(res));
      response_buffer_free(&response);
      return ST_ERR_NETWORK;
   }

   if (http_code != 200) {
      /* Don't log response body - may contain sensitive error details */
      LOG_ERROR("SmartThings: Token refresh failed (HTTP %ld)", http_code);
      response_buffer_free(&response);
      return ST_ERR_TOKEN_EXPIRED;
   }

   /* Parse response */
   json_object *root = json_tokener_parse(response.data);
   response_buffer_free(&response);

   if (!root) {
      LOG_ERROR("SmartThings: Failed to parse token response");
      return ST_ERR_API;
   }

   json_object *obj;
   if (json_object_object_get_ex(root, "access_token", &obj)) {
      strncpy(s_state.tokens.access_token, json_object_get_string(obj),
              sizeof(s_state.tokens.access_token) - 1);
   }
   if (json_object_object_get_ex(root, "refresh_token", &obj)) {
      strncpy(s_state.tokens.refresh_token, json_object_get_string(obj),
              sizeof(s_state.tokens.refresh_token) - 1);
   }

   int expires_in = 86400; /* Default 24 hours */
   if (json_object_object_get_ex(root, "expires_in", &obj)) {
      expires_in = json_object_get_int(obj);
   }
   s_state.tokens.token_expiry = time(NULL) + expires_in;

   json_object_put(root);

   /* Persist tokens */
   save_tokens_to_file();

   LOG_INFO("SmartThings: Token refreshed, expires in %d seconds", expires_in);
   return ST_OK;
}

/**
 * @brief Ensure we have a valid access token, refreshing if necessary
 *
 * Uses a dedicated mutex for token refresh to avoid rwlock upgrade race.
 * The refresh is serialized but API calls can proceed concurrently.
 */
static st_error_t ensure_valid_token(void) {
   if (!s_state.tokens.access_token[0]) {
      return ST_ERR_NOT_AUTHENTICATED;
   }

   /* PAT mode: no refresh needed, token is considered always valid */
   if (s_state.auth_mode == AUTH_PAT) {
      return ST_OK;
   }

   /* OAuth2 mode: check expiry and refresh if needed */
   int64_t now = time(NULL);
   if (now + TOKEN_REFRESH_MARGIN_SEC >= s_state.tokens.token_expiry) {
      /*
       * Use dedicated mutex for refresh - avoids the problematic rwlock
       * upgrade pattern that has a race window.
       */
      pthread_mutex_lock(&s_state.token_refresh_mutex);

      /* Double-check after acquiring mutex - another thread may have refreshed */
      now = time(NULL);
      if (now + TOKEN_REFRESH_MARGIN_SEC >= s_state.tokens.token_expiry) {
         st_error_t err = refresh_access_token();
         pthread_mutex_unlock(&s_state.token_refresh_mutex);
         return err;
      }

      pthread_mutex_unlock(&s_state.token_refresh_mutex);
   }

   return ST_OK;
}

/* =============================================================================
 * Capability Parsing
 * ============================================================================= */

/* Static lookup table for capability name -> bitmask (sorted for binary search) */
static const struct {
   const char *name;
   st_capability_t cap;
} capability_map[] = {
   { "battery", ST_CAP_BATTERY },
   { "colorControl", ST_CAP_COLOR_CONTROL },
   { "colorTemperature", ST_CAP_COLOR_TEMP },
   { "contactSensor", ST_CAP_CONTACT },
   { "fanSpeed", ST_CAP_FAN_SPEED },
   { "lock", ST_CAP_LOCK },
   { "motionSensor", ST_CAP_MOTION },
   { "powerMeter", ST_CAP_POWER_METER },
   { "presenceSensor", ST_CAP_PRESENCE },
   { "relativeHumidityMeasurement", ST_CAP_HUMIDITY },
   { "switch", ST_CAP_SWITCH },
   { "switchLevel", ST_CAP_SWITCH_LEVEL },
   { "temperatureMeasurement", ST_CAP_TEMPERATURE },
   { "thermostatCoolingSetpoint", ST_CAP_THERMOSTAT },
   { "thermostatHeatingSetpoint", ST_CAP_THERMOSTAT },
   { "windowShade", ST_CAP_WINDOW_SHADE },
};
#define CAPABILITY_MAP_SIZE (sizeof(capability_map) / sizeof(capability_map[0]))

static int capability_compare(const void *key, const void *entry) {
   return strcmp((const char *)key, ((const struct {
                                       const char *name;
                                       st_capability_t cap;
                                    } *)entry)
                                        ->name);
}

static st_capability_t lookup_capability(const char *name) {
   void *result = bsearch(name, capability_map, CAPABILITY_MAP_SIZE, sizeof(capability_map[0]),
                          capability_compare);
   if (result) {
      return ((const struct {
                const char *name;
                st_capability_t cap;
             } *)result)
          ->cap;
   }
   return ST_CAP_NONE;
}

static uint32_t parse_capabilities(json_object *components) {
   uint32_t caps = ST_CAP_NONE;

   if (!components || !json_object_is_type(components, json_type_array))
      return caps;

   int len = json_object_array_length(components);
   for (int i = 0; i < len; i++) {
      json_object *comp = json_object_array_get_idx(components, i);
      json_object *capabilities;
      if (!json_object_object_get_ex(comp, "capabilities", &capabilities))
         continue;

      int cap_len = json_object_array_length(capabilities);
      for (int j = 0; j < cap_len; j++) {
         json_object *cap_obj = json_object_array_get_idx(capabilities, j);
         json_object *id_obj;
         if (!json_object_object_get_ex(cap_obj, "id", &id_obj))
            continue;

         const char *id = json_object_get_string(id_obj);
         caps |= lookup_capability(id);
      }
   }

   return caps;
}

/* =============================================================================
 * Fuzzy String Matching
 * ============================================================================= */
static void str_tolower(char *dst, const char *src, size_t max_len) {
   size_t i;
   for (i = 0; i < max_len - 1 && src[i]; i++) {
      dst[i] = tolower((unsigned char)src[i]);
   }
   dst[i] = '\0';
}

static int fuzzy_match_score(const char *haystack, const char *needle) {
   char hay_lower[256], needle_lower[256];
   str_tolower(hay_lower, haystack, sizeof(hay_lower));
   str_tolower(needle_lower, needle, sizeof(needle_lower));

   /* Exact match */
   if (strcmp(hay_lower, needle_lower) == 0)
      return 100;

   /* Contains match */
   if (strstr(hay_lower, needle_lower))
      return 80;

   /* Word-by-word match (score by overlap) */
   int score = 0;
   char needle_copy[256];
   strncpy(needle_copy, needle_lower, sizeof(needle_copy) - 1);
   needle_copy[sizeof(needle_copy) - 1] = '\0';

   char *token = strtok(needle_copy, " ");
   while (token) {
      if (strstr(hay_lower, token))
         score += 20;
      token = strtok(NULL, " ");
   }

   return score;
}

/* =============================================================================
 * Device Command Helper
 * ============================================================================= */

/**
 * @brief Send a simple command without arguments using a pre-formatted JSON template
 *
 * This is more efficient than building JSON objects for common on/off commands.
 */
static st_error_t send_simple_command(const char *device_id,
                                      const char *capability,
                                      const char *command) {
   if (!device_id || !capability || !command)
      return ST_ERR_INVALID_PARAM;

   /* Security: Validate device_id is a valid UUID format (prevents URL injection) */
   if (!is_valid_device_id(device_id)) {
      LOG_ERROR("SmartThings: Invalid device ID format");
      return ST_ERR_INVALID_PARAM;
   }

   st_error_t err = ensure_valid_token();
   if (err != ST_OK) {
      return err;
   }

   /* Use pre-formatted JSON template for simple commands (no arguments) */
   char body[256];
   snprintf(body, sizeof(body),
            "{\"commands\":[{\"component\":\"main\",\"capability\":\"%s\","
            "\"command\":\"%s\"}]}",
            capability, command);

   char url[512];
   snprintf(url, sizeof(url), "%s/devices/%s/commands", ST_API_BASE_URL, device_id);

   response_buffer_t response;
   response_buffer_init(&response);
   long http_code;

   err = do_api_request("POST", url, body, &response, &http_code);

   response_buffer_free(&response);

   if (err == ST_OK) {
      LOG_INFO("SmartThings: Sent %s.%s to device %s", capability, command, device_id);
   }

   return err;
}

static st_error_t send_device_command(const char *device_id,
                                      const char *capability,
                                      const char *command,
                                      json_object *args) {
   if (!device_id || !capability || !command)
      return ST_ERR_INVALID_PARAM;

   /* Security: Validate device_id is a valid UUID format (prevents URL injection) */
   if (!is_valid_device_id(device_id)) {
      LOG_ERROR("SmartThings: Invalid device ID format");
      return ST_ERR_INVALID_PARAM;
   }

   /* Use simple command path if no arguments */
   if (!args) {
      return send_simple_command(device_id, capability, command);
   }

   st_error_t err = ensure_valid_token();
   if (err != ST_OK) {
      return err;
   }

   /* Build command JSON */
   json_object *cmd = json_object_new_object();
   json_object_object_add(cmd, "component", json_object_new_string("main"));
   json_object_object_add(cmd, "capability", json_object_new_string(capability));
   json_object_object_add(cmd, "command", json_object_new_string(command));
   json_object_object_add(cmd, "arguments", json_object_get(args));

   json_object *commands = json_object_new_array();
   json_object_array_add(commands, cmd);

   json_object *root = json_object_new_object();
   json_object_object_add(root, "commands", commands);

   const char *body = json_object_to_json_string(root);

   char url[512];
   snprintf(url, sizeof(url), "%s/devices/%s/commands", ST_API_BASE_URL, device_id);

   response_buffer_t response;
   response_buffer_init(&response);
   long http_code;

   err = do_api_request("POST", url, body, &response, &http_code);

   json_object_put(root);
   response_buffer_free(&response);

   if (err == ST_OK) {
      LOG_INFO("SmartThings: Sent %s.%s to device %s", capability, command, device_id);
   }

   return err;
}

/* =============================================================================
 * Public API - Lifecycle
 * ============================================================================= */
st_error_t smartthings_init(void) {
   if (s_state.initialized)
      return ST_OK;

   const secrets_config_t *secrets = config_get_secrets();
   if (!secrets) {
      LOG_INFO("SmartThings: Not configured (no secrets)");
      return ST_ERR_NOT_CONFIGURED;
   }

   pthread_rwlock_init(&s_state.rwlock, NULL);
   pthread_mutex_init(&s_state.token_refresh_mutex, NULL);

   /* Check for PAT mode first (simpler, preferred) */
   if (secrets->smartthings_access_token[0]) {
      s_state.auth_mode = AUTH_PAT;
      strncpy(s_state.tokens.access_token, secrets->smartthings_access_token,
              sizeof(s_state.tokens.access_token) - 1);
      s_state.tokens.token_expiry = INT64_MAX; /* PAT doesn't expire (from our perspective) */
      s_state.initialized = true;
      LOG_INFO("SmartThings: Initialized with Personal Access Token");

      /* Invalidate cached system instructions so LLM prompt includes SmartThings */
      invalidate_system_instructions();
      return ST_OK;
   }

   /* Check for OAuth2 mode */
   if (secrets->smartthings_client_id[0] && secrets->smartthings_client_secret[0]) {
      s_state.auth_mode = AUTH_OAUTH2;

      /* Get secure token file path */
      if (get_token_file_path(s_state.token_file_path, sizeof(s_state.token_file_path)) != 0) {
         LOG_WARNING(
             "SmartThings: Cannot determine token storage path, OAuth2 tokens won't persist");
      }

      /* Try to load existing tokens from file */
      if (s_state.token_file_path[0]) {
         load_tokens_from_file();
      }

      s_state.initialized = true;
      LOG_INFO("SmartThings: Initialized with OAuth2 credentials");

      /* Invalidate cached system instructions so LLM prompt includes SmartThings */
      if (s_state.tokens.access_token[0]) {
         invalidate_system_instructions();
      }
      return ST_OK;
   }

   LOG_INFO("SmartThings: Not configured (no PAT or OAuth2 credentials)");
   return ST_ERR_NOT_CONFIGURED;
}

void smartthings_cleanup(void) {
   if (!s_state.initialized)
      return;

   /* Cleanup CURL handle */
   if (s_state.curl_handle) {
      curl_easy_cleanup(s_state.curl_handle);
      s_state.curl_handle = NULL;
   }

   pthread_rwlock_destroy(&s_state.rwlock);
   pthread_mutex_destroy(&s_state.token_refresh_mutex);

   /* Securely zero sensitive token data */
   secure_zero(&s_state.tokens, sizeof(s_state.tokens));
   secure_zero(s_state.pending_oauth_state, sizeof(s_state.pending_oauth_state));

   /* Zero remaining non-sensitive state */
   s_state.initialized = false;
   s_state.auth_mode = AUTH_NONE;
   memset(&s_state.device_cache, 0, sizeof(s_state.device_cache));
   s_state.token_file_path[0] = '\0';

   LOG_INFO("SmartThings: Service cleaned up");
}

bool smartthings_is_configured(void) {
   const secrets_config_t *secrets = config_get_secrets();
   if (!secrets)
      return false;

   /* Configured if either PAT or OAuth2 credentials are set */
   return secrets->smartthings_access_token[0] ||
          (secrets->smartthings_client_id[0] && secrets->smartthings_client_secret[0]);
}

bool smartthings_is_authenticated(void) {
   if (!s_state.initialized)
      return false;
   return s_state.tokens.access_token[0] != '\0';
}

st_error_t smartthings_get_status(st_status_t *status) {
   if (!status)
      return ST_ERR_INVALID_PARAM;

   memset(status, 0, sizeof(*status));

   if (!s_state.initialized) {
      return ST_OK;
   }

   pthread_rwlock_rdlock(&s_state.rwlock);
   status->has_tokens = s_state.tokens.access_token[0] != '\0';
   status->tokens_valid = status->has_tokens && (time(NULL) < s_state.tokens.token_expiry);
   status->token_expiry = s_state.tokens.token_expiry;
   status->devices_count = s_state.device_cache.count;

   /* Map internal auth mode to public enum */
   switch (s_state.auth_mode) {
      case AUTH_PAT:
         status->auth_mode = ST_AUTH_MODE_PAT;
         break;
      case AUTH_OAUTH2:
         status->auth_mode = ST_AUTH_MODE_OAUTH2;
         break;
      default:
         status->auth_mode = ST_AUTH_MODE_NONE;
         break;
   }
   pthread_rwlock_unlock(&s_state.rwlock);

   return ST_OK;
}

/* =============================================================================
 * Public API - OAuth2
 * ============================================================================= */

/**
 * @brief Generate a cryptographically random state for OAuth CSRF protection
 */
static st_error_t generate_oauth_state(char *state_buf, size_t buf_size) {
   if (buf_size < OAUTH_STATE_LEN + 1)
      return ST_ERR_INVALID_PARAM;

   unsigned char random_bytes[16];
   ssize_t result = getrandom(random_bytes, sizeof(random_bytes), 0);
   if (result != sizeof(random_bytes)) {
      LOG_ERROR("SmartThings: Failed to generate random state");
      return ST_ERR_MEMORY;
   }

   for (size_t i = 0; i < sizeof(random_bytes); i++) {
      snprintf(&state_buf[i * 2], 3, "%02x", random_bytes[i]);
   }
   state_buf[OAUTH_STATE_LEN] = '\0';

   return ST_OK;
}

st_error_t smartthings_get_auth_url(const char *redirect_uri, char *url_buf, size_t buf_size) {
   if (!redirect_uri || !url_buf || buf_size < 512)
      return ST_ERR_INVALID_PARAM;

   const secrets_config_t *secrets = config_get_secrets();
   if (!secrets || !secrets->smartthings_client_id[0])
      return ST_ERR_NOT_CONFIGURED;

   /* Generate CSRF state and store for later verification */
   st_error_t err = generate_oauth_state(s_state.pending_oauth_state,
                                         sizeof(s_state.pending_oauth_state));
   if (err != ST_OK) {
      return err;
   }

   CURL *curl = get_curl_handle();
   if (!curl)
      return ST_ERR_MEMORY;

   char *encoded_redirect = curl_easy_escape(curl, redirect_uri, 0);
   char *encoded_scope = curl_easy_escape(curl, ST_OAUTH_SCOPES, 0);
   char *encoded_state = curl_easy_escape(curl, s_state.pending_oauth_state, 0);

   snprintf(url_buf, buf_size,
            "%s?response_type=code&client_id=%s&redirect_uri=%s&scope=%s&state=%s", ST_AUTH_URL,
            secrets->smartthings_client_id, encoded_redirect, encoded_scope, encoded_state);

   curl_free(encoded_redirect);
   curl_free(encoded_scope);
   curl_free(encoded_state);
   /* Note: Don't cleanup curl handle - it's reused */

   LOG_INFO("SmartThings: Generated OAuth URL with CSRF state");
   return ST_OK;
}

st_error_t smartthings_exchange_code(const char *auth_code,
                                     const char *redirect_uri,
                                     const char *state) {
   if (!auth_code || !redirect_uri)
      return ST_ERR_INVALID_PARAM;

   /* CSRF protection: Verify state matches what we generated */
   if (state && s_state.pending_oauth_state[0]) {
      if (strcmp(state, s_state.pending_oauth_state) != 0) {
         LOG_ERROR("SmartThings: OAuth state mismatch - possible CSRF attack");
         secure_zero(s_state.pending_oauth_state, sizeof(s_state.pending_oauth_state));
         return ST_ERR_INVALID_PARAM;
      }
      /* Clear the used state */
      secure_zero(s_state.pending_oauth_state, sizeof(s_state.pending_oauth_state));
   }

   const secrets_config_t *secrets = config_get_secrets();
   if (!secrets || !secrets->smartthings_client_id[0] || !secrets->smartthings_client_secret[0]) {
      return ST_ERR_NOT_CONFIGURED;
   }

   LOG_INFO("SmartThings: Exchanging auth code for tokens...");

   CURL *curl = get_curl_handle();
   if (!curl)
      return ST_ERR_MEMORY;

   response_buffer_t response;
   response_buffer_init(&response);

   /* Build POST body */
   char *encoded_code = curl_easy_escape(curl, auth_code, 0);
   char *encoded_redirect = curl_easy_escape(curl, redirect_uri, 0);
   char *encoded_client_id = curl_easy_escape(curl, secrets->smartthings_client_id, 0);
   char *encoded_client_secret = curl_easy_escape(curl, secrets->smartthings_client_secret, 0);

   char post_data[2048];
   snprintf(post_data, sizeof(post_data),
            "grant_type=authorization_code&code=%s&redirect_uri=%s&client_id=%s&client_secret=%s",
            encoded_code, encoded_redirect, encoded_client_id, encoded_client_secret);

   curl_free(encoded_code);
   curl_free(encoded_redirect);
   curl_free(encoded_client_id);
   curl_free(encoded_client_secret);

   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

   configure_curl_security(curl);
   curl_easy_setopt(curl, CURLOPT_URL, ST_TOKEN_URL);
   curl_easy_setopt(curl, CURLOPT_POST, 1L);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

   CURLcode res = curl_easy_perform(curl);
   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

   curl_slist_free_all(headers);
   /* Note: Don't cleanup curl handle - it's reused */

   /* Securely clear post_data containing client secret */
   secure_zero(post_data, sizeof(post_data));

   if (res != CURLE_OK) {
      LOG_ERROR("SmartThings: Code exchange failed: %s", curl_easy_strerror(res));
      response_buffer_free(&response);
      return ST_ERR_NETWORK;
   }

   if (http_code != 200) {
      /* Don't log response body - may contain sensitive error details */
      LOG_ERROR("SmartThings: Code exchange failed (HTTP %ld)", http_code);
      response_buffer_free(&response);
      return ST_ERR_API;
   }

   /* Parse response */
   json_object *root = json_tokener_parse(response.data);
   response_buffer_free(&response);

   if (!root) {
      LOG_ERROR("SmartThings: Failed to parse token response");
      return ST_ERR_API;
   }

   pthread_rwlock_wrlock(&s_state.rwlock);

   json_object *obj;
   if (json_object_object_get_ex(root, "access_token", &obj)) {
      strncpy(s_state.tokens.access_token, json_object_get_string(obj),
              sizeof(s_state.tokens.access_token) - 1);
   }
   if (json_object_object_get_ex(root, "refresh_token", &obj)) {
      strncpy(s_state.tokens.refresh_token, json_object_get_string(obj),
              sizeof(s_state.tokens.refresh_token) - 1);
   }

   int expires_in = 86400;
   if (json_object_object_get_ex(root, "expires_in", &obj)) {
      expires_in = json_object_get_int(obj);
   }
   s_state.tokens.token_expiry = time(NULL) + expires_in;

   json_object_put(root);

   save_tokens_to_file();
   pthread_rwlock_unlock(&s_state.rwlock);

   LOG_INFO("SmartThings: Successfully authenticated");

   /* Invalidate cached system instructions so LLM prompt includes SmartThings */
   invalidate_system_instructions();
   return ST_OK;
}

st_error_t smartthings_disconnect(void) {
   if (!s_state.initialized)
      return ST_OK;

   pthread_rwlock_wrlock(&s_state.rwlock);

   /* Securely clear sensitive token data */
   secure_zero(&s_state.tokens, sizeof(s_state.tokens));
   memset(&s_state.device_cache, 0, sizeof(s_state.device_cache));

   /* Delete token file */
   if (s_state.token_file_path[0]) {
      unlink(s_state.token_file_path);
   }

   pthread_rwlock_unlock(&s_state.rwlock);

   LOG_INFO("SmartThings: Disconnected");

   /* Invalidate cached system instructions so LLM prompt removes SmartThings */
   invalidate_system_instructions();
   return ST_OK;
}

/* =============================================================================
 * Public API - Device Discovery
 * ============================================================================= */
st_error_t smartthings_list_devices(const st_device_list_t **list) {
   if (!list)
      return ST_ERR_INVALID_PARAM;

   if (!s_state.initialized)
      return ST_ERR_NOT_CONFIGURED;

   pthread_rwlock_rdlock(&s_state.rwlock);

   /* Check cache validity */
   int64_t now = time(NULL);
   if (s_state.device_cache.count > 0 &&
       (now - s_state.device_cache.cached_at) < ST_DEVICE_CACHE_TTL_SEC) {
      *list = &s_state.device_cache;
      pthread_rwlock_unlock(&s_state.rwlock);
      return ST_OK;
   }

   pthread_rwlock_unlock(&s_state.rwlock);

   /* Cache expired or empty, refresh */
   return smartthings_refresh_devices(list);
}

st_error_t smartthings_refresh_devices(const st_device_list_t **list) {
   if (!list)
      return ST_ERR_INVALID_PARAM;

   if (!s_state.initialized)
      return ST_ERR_NOT_CONFIGURED;

   pthread_rwlock_rdlock(&s_state.rwlock);
   st_error_t err = ensure_valid_token();
   if (err != ST_OK) {
      pthread_rwlock_unlock(&s_state.rwlock);
      return err;
   }

   char url[256];
   snprintf(url, sizeof(url), "%s/devices", ST_API_BASE_URL);

   response_buffer_t response;
   response_buffer_init(&response);
   long http_code;

   err = do_api_request("GET", url, NULL, &response, &http_code);
   pthread_rwlock_unlock(&s_state.rwlock);

   if (err != ST_OK) {
      response_buffer_free(&response);
      return err;
   }

   /* Parse device list */
   json_object *root = json_tokener_parse(response.data);
   response_buffer_free(&response);

   if (!root) {
      LOG_ERROR("SmartThings: Failed to parse device list");
      return ST_ERR_API;
   }

   pthread_rwlock_wrlock(&s_state.rwlock);

   json_object *items;
   if (!json_object_object_get_ex(root, "items", &items)) {
      json_object_put(root);
      pthread_rwlock_unlock(&s_state.rwlock);
      return ST_ERR_API;
   }

   int count = json_object_array_length(items);
   s_state.device_cache.count = 0;

   for (int i = 0; i < count && s_state.device_cache.count < ST_MAX_DEVICES; i++) {
      json_object *dev = json_object_array_get_idx(items, i);
      st_device_t *out = &s_state.device_cache.devices[s_state.device_cache.count];
      memset(out, 0, sizeof(*out));

      json_object *obj;
      if (json_object_object_get_ex(dev, "deviceId", &obj)) {
         strncpy(out->id, json_object_get_string(obj), sizeof(out->id) - 1);
      }
      if (json_object_object_get_ex(dev, "name", &obj)) {
         strncpy(out->name, json_object_get_string(obj), sizeof(out->name) - 1);
      }
      if (json_object_object_get_ex(dev, "label", &obj)) {
         const char *label = json_object_get_string(obj);
         if (label && label[0]) {
            strncpy(out->label, label, sizeof(out->label) - 1);
         } else {
            strncpy(out->label, out->name, sizeof(out->label) - 1);
         }
      } else {
         strncpy(out->label, out->name, sizeof(out->label) - 1);
      }
      if (json_object_object_get_ex(dev, "roomId", &obj)) {
         /* Room ID would need another API call to get room name */
         /* For now just store the ID */
         strncpy(out->room, json_object_get_string(obj), sizeof(out->room) - 1);
      }

      json_object *components;
      if (json_object_object_get_ex(dev, "components", &components)) {
         out->capabilities = parse_capabilities(components);
      }

      s_state.device_cache.count++;
   }

   s_state.device_cache.cached_at = time(NULL);
   json_object_put(root);

   *list = &s_state.device_cache;
   pthread_rwlock_unlock(&s_state.rwlock);

   LOG_INFO("SmartThings: Loaded %d devices", s_state.device_cache.count);
   return ST_OK;
}

st_error_t smartthings_find_device(const char *friendly_name, const st_device_t **device) {
   if (!friendly_name || !device)
      return ST_ERR_INVALID_PARAM;

   const st_device_list_t *list;
   st_error_t err = smartthings_list_devices(&list);
   if (err != ST_OK)
      return err;

   int best_score = 0;
   const st_device_t *best_match = NULL;

   pthread_rwlock_rdlock(&s_state.rwlock);
   for (int i = 0; i < list->count; i++) {
      const st_device_t *dev = &list->devices[i];

      /* Try label first, then name */
      int score = fuzzy_match_score(dev->label, friendly_name);
      int name_score = fuzzy_match_score(dev->name, friendly_name);
      if (name_score > score)
         score = name_score;

      if (score > best_score) {
         best_score = score;
         best_match = dev;
      }
   }
   pthread_rwlock_unlock(&s_state.rwlock);

   if (best_score < 40 || !best_match) {
      LOG_WARNING("SmartThings: No device matching '%s' (best score: %d)", friendly_name,
                  best_score);
      return ST_ERR_DEVICE_NOT_FOUND;
   }

   *device = best_match;
   LOG_INFO("SmartThings: Matched '%s' to device '%s' (score: %d)", friendly_name,
            best_match->label, best_score);
   return ST_OK;
}

st_error_t smartthings_get_device_status(const char *device_id, st_device_state_t *state) {
   if (!device_id || !state)
      return ST_ERR_INVALID_PARAM;

   if (!s_state.initialized)
      return ST_ERR_NOT_CONFIGURED;

   pthread_rwlock_rdlock(&s_state.rwlock);
   st_error_t err = ensure_valid_token();
   if (err != ST_OK) {
      pthread_rwlock_unlock(&s_state.rwlock);
      return err;
   }

   char url[512];
   snprintf(url, sizeof(url), "%s/devices/%s/status", ST_API_BASE_URL, device_id);

   response_buffer_t response;
   response_buffer_init(&response);
   long http_code;

   err = do_api_request("GET", url, NULL, &response, &http_code);
   pthread_rwlock_unlock(&s_state.rwlock);

   if (err != ST_OK) {
      response_buffer_free(&response);
      return err;
   }

   /* Parse status response */
   json_object *root = json_tokener_parse(response.data);
   response_buffer_free(&response);

   if (!root) {
      return ST_ERR_API;
   }

   memset(state, 0, sizeof(*state));

   /* Navigate: components.main.<capability>.<attribute>.value */
   json_object *components, *main_comp;
   if (json_object_object_get_ex(root, "components", &components) &&
       json_object_object_get_ex(components, "main", &main_comp)) {
      json_object *cap, *attr, *val;

      /* switch */
      if (json_object_object_get_ex(main_comp, "switch", &cap) &&
          json_object_object_get_ex(cap, "switch", &attr) &&
          json_object_object_get_ex(attr, "value", &val)) {
         state->switch_on = strcmp(json_object_get_string(val), "on") == 0;
      }

      /* switchLevel */
      if (json_object_object_get_ex(main_comp, "switchLevel", &cap) &&
          json_object_object_get_ex(cap, "level", &attr) &&
          json_object_object_get_ex(attr, "value", &val)) {
         state->level = json_object_get_int(val);
      }

      /* colorControl */
      if (json_object_object_get_ex(main_comp, "colorControl", &cap)) {
         if (json_object_object_get_ex(cap, "hue", &attr) &&
             json_object_object_get_ex(attr, "value", &val)) {
            state->hue = json_object_get_int(val);
         }
         if (json_object_object_get_ex(cap, "saturation", &attr) &&
             json_object_object_get_ex(attr, "value", &val)) {
            state->saturation = json_object_get_int(val);
         }
      }

      /* colorTemperature */
      if (json_object_object_get_ex(main_comp, "colorTemperature", &cap) &&
          json_object_object_get_ex(cap, "colorTemperature", &attr) &&
          json_object_object_get_ex(attr, "value", &val)) {
         state->color_temp = json_object_get_int(val);
      }

      /* temperatureMeasurement */
      if (json_object_object_get_ex(main_comp, "temperatureMeasurement", &cap) &&
          json_object_object_get_ex(cap, "temperature", &attr) &&
          json_object_object_get_ex(attr, "value", &val)) {
         state->temperature = json_object_get_double(val);
      }

      /* relativeHumidityMeasurement */
      if (json_object_object_get_ex(main_comp, "relativeHumidityMeasurement", &cap) &&
          json_object_object_get_ex(cap, "humidity", &attr) &&
          json_object_object_get_ex(attr, "value", &val)) {
         state->humidity = json_object_get_double(val);
      }

      /* battery */
      if (json_object_object_get_ex(main_comp, "battery", &cap) &&
          json_object_object_get_ex(cap, "battery", &attr) &&
          json_object_object_get_ex(attr, "value", &val)) {
         state->battery = json_object_get_int(val);
      }

      /* lock */
      if (json_object_object_get_ex(main_comp, "lock", &cap) &&
          json_object_object_get_ex(cap, "lock", &attr) &&
          json_object_object_get_ex(attr, "value", &val)) {
         state->locked = strcmp(json_object_get_string(val), "locked") == 0;
      }

      /* motionSensor */
      if (json_object_object_get_ex(main_comp, "motionSensor", &cap) &&
          json_object_object_get_ex(cap, "motion", &attr) &&
          json_object_object_get_ex(attr, "value", &val)) {
         state->motion_active = strcmp(json_object_get_string(val), "active") == 0;
      }

      /* contactSensor */
      if (json_object_object_get_ex(main_comp, "contactSensor", &cap) &&
          json_object_object_get_ex(cap, "contact", &attr) &&
          json_object_object_get_ex(attr, "value", &val)) {
         state->contact_open = strcmp(json_object_get_string(val), "open") == 0;
      }

      /* presenceSensor */
      if (json_object_object_get_ex(main_comp, "presenceSensor", &cap) &&
          json_object_object_get_ex(cap, "presence", &attr) &&
          json_object_object_get_ex(attr, "value", &val)) {
         state->present = strcmp(json_object_get_string(val), "present") == 0;
      }

      /* windowShade */
      if (json_object_object_get_ex(main_comp, "windowShade", &cap) &&
          json_object_object_get_ex(cap, "shadeLevel", &attr) &&
          json_object_object_get_ex(attr, "value", &val)) {
         state->shade_level = json_object_get_int(val);
      }

      /* fanSpeed */
      if (json_object_object_get_ex(main_comp, "fanSpeed", &cap) &&
          json_object_object_get_ex(cap, "fanSpeed", &attr) &&
          json_object_object_get_ex(attr, "value", &val)) {
         state->fan_speed = json_object_get_int(val);
      }

      /* powerMeter */
      if (json_object_object_get_ex(main_comp, "powerMeter", &cap) &&
          json_object_object_get_ex(cap, "power", &attr) &&
          json_object_object_get_ex(attr, "value", &val)) {
         state->power = json_object_get_double(val);
      }
   }

   json_object_put(root);
   return ST_OK;
}

/* =============================================================================
 * Public API - Device Control
 * ============================================================================= */
st_error_t smartthings_switch_on(const char *device_id) {
   return send_device_command(device_id, "switch", "on", NULL);
}

st_error_t smartthings_switch_off(const char *device_id) {
   return send_device_command(device_id, "switch", "off", NULL);
}

st_error_t smartthings_set_level(const char *device_id, int level) {
   if (level < 0)
      level = 0;
   if (level > 100)
      level = 100;

   json_object *args = json_object_new_array();
   json_object_array_add(args, json_object_new_int(level));

   st_error_t err = send_device_command(device_id, "switchLevel", "setLevel", args);
   json_object_put(args);
   return err;
}

st_error_t smartthings_set_color(const char *device_id, int hue, int saturation) {
   if (hue < 0)
      hue = 0;
   if (hue > 100)
      hue = 100;
   if (saturation < 0)
      saturation = 0;
   if (saturation > 100)
      saturation = 100;

   json_object *color = json_object_new_object();
   json_object_object_add(color, "hue", json_object_new_int(hue));
   json_object_object_add(color, "saturation", json_object_new_int(saturation));

   json_object *args = json_object_new_array();
   json_object_array_add(args, color);

   st_error_t err = send_device_command(device_id, "colorControl", "setColor", args);
   json_object_put(args);
   return err;
}

st_error_t smartthings_set_color_temp(const char *device_id, int kelvin) {
   json_object *args = json_object_new_array();
   json_object_array_add(args, json_object_new_int(kelvin));

   st_error_t err = send_device_command(device_id, "colorTemperature", "setColorTemperature", args);
   json_object_put(args);
   return err;
}

st_error_t smartthings_lock(const char *device_id) {
   return send_device_command(device_id, "lock", "lock", NULL);
}

st_error_t smartthings_unlock(const char *device_id) {
   return send_device_command(device_id, "lock", "unlock", NULL);
}

st_error_t smartthings_set_thermostat(const char *device_id, double temp_f) {
   json_object *args = json_object_new_array();
   json_object_array_add(args, json_object_new_double(temp_f));

   st_error_t err = send_device_command(device_id, "thermostatCoolingSetpoint",
                                        "setCoolingSetpoint", args);
   json_object_put(args);
   return err;
}

st_error_t smartthings_set_shade_level(const char *device_id, int level) {
   if (level < 0)
      level = 0;
   if (level > 100)
      level = 100;

   json_object *args = json_object_new_array();
   json_object_array_add(args, json_object_new_int(level));

   st_error_t err = send_device_command(device_id, "windowShade", "setShadeLevel", args);
   json_object_put(args);
   return err;
}

st_error_t smartthings_set_fan_speed(const char *device_id, int speed) {
   if (speed < 0)
      speed = 0;
   if (speed > 4)
      speed = 4;

   json_object *args = json_object_new_array();
   json_object_array_add(args, json_object_new_int(speed));

   st_error_t err = send_device_command(device_id, "fanSpeed", "setFanSpeed", args);
   json_object_put(args);
   return err;
}

/* =============================================================================
 * Public API - Utilities
 * ============================================================================= */
const char *smartthings_error_str(st_error_t err) {
   switch (err) {
      case ST_OK:
         return "Success";
      case ST_ERR_NOT_CONFIGURED:
         return "SmartThings not configured";
      case ST_ERR_NOT_AUTHENTICATED:
         return "Not authenticated";
      case ST_ERR_TOKEN_EXPIRED:
         return "Token expired";
      case ST_ERR_NETWORK:
         return "Network error";
      case ST_ERR_API:
         return "API error";
      case ST_ERR_DEVICE_NOT_FOUND:
         return "Device not found";
      case ST_ERR_INVALID_CAPABILITY:
         return "Invalid capability";
      case ST_ERR_RATE_LIMITED:
         return "Rate limited";
      case ST_ERR_INVALID_PARAM:
         return "Invalid parameter";
      case ST_ERR_MEMORY:
         return "Memory allocation failed";
      default:
         return "Unknown error";
   }
}

const char *smartthings_capability_str(st_capability_t cap) {
   switch (cap) {
      case ST_CAP_SWITCH:
         return "switch";
      case ST_CAP_SWITCH_LEVEL:
         return "dimmer";
      case ST_CAP_COLOR_CONTROL:
         return "color";
      case ST_CAP_COLOR_TEMP:
         return "color temperature";
      case ST_CAP_THERMOSTAT:
         return "thermostat";
      case ST_CAP_LOCK:
         return "lock";
      case ST_CAP_MOTION:
         return "motion sensor";
      case ST_CAP_CONTACT:
         return "contact sensor";
      case ST_CAP_TEMPERATURE:
         return "temperature";
      case ST_CAP_HUMIDITY:
         return "humidity";
      case ST_CAP_BATTERY:
         return "battery";
      case ST_CAP_POWER_METER:
         return "power meter";
      case ST_CAP_PRESENCE:
         return "presence";
      case ST_CAP_WINDOW_SHADE:
         return "shade";
      case ST_CAP_FAN_SPEED:
         return "fan";
      default:
         return "unknown";
   }
}

const char *smartthings_auth_mode_str(st_auth_mode_t mode) {
   switch (mode) {
      case ST_AUTH_MODE_PAT:
         return "pat";
      case ST_AUTH_MODE_OAUTH2:
         return "oauth2";
      default:
         return "none";
   }
}

int smartthings_device_to_json(const st_device_t *device, char *buf, size_t buf_size) {
   if (!device || !buf || buf_size < 128)
      return -1;

   json_object *obj = json_object_new_object();
   json_object_object_add(obj, "id", json_object_new_string(device->id));
   json_object_object_add(obj, "name", json_object_new_string(device->name));
   json_object_object_add(obj, "label", json_object_new_string(device->label));
   json_object_object_add(obj, "room", json_object_new_string(device->room));

   /* Build capabilities array */
   json_object *caps = json_object_new_array();
   for (int i = 0; i < 15; i++) {
      st_capability_t cap = (st_capability_t)(1 << i);
      if (device->capabilities & cap) {
         json_object_array_add(caps, json_object_new_string(smartthings_capability_str(cap)));
      }
   }
   json_object_object_add(obj, "capabilities", caps);

   const char *json_str = json_object_to_json_string(obj);
   int len = snprintf(buf, buf_size, "%s", json_str);

   json_object_put(obj);
   return len;
}

int smartthings_list_to_json(const st_device_list_t *list, char *buf, size_t buf_size) {
   if (!list || !buf || buf_size < 64)
      return -1;

   json_object *root = json_object_new_object();
   json_object *devices = json_object_new_array();

   for (int i = 0; i < list->count; i++) {
      const st_device_t *dev = &list->devices[i];
      json_object *obj = json_object_new_object();

      json_object_object_add(obj, "id", json_object_new_string(dev->id));
      json_object_object_add(obj, "name", json_object_new_string(dev->name));
      json_object_object_add(obj, "label", json_object_new_string(dev->label));

      json_object *caps = json_object_new_array();
      for (int j = 0; j < 15; j++) {
         st_capability_t cap = (st_capability_t)(1 << j);
         if (dev->capabilities & cap) {
            json_object_array_add(caps, json_object_new_string(smartthings_capability_str(cap)));
         }
      }
      json_object_object_add(obj, "capabilities", caps);

      json_object_array_add(devices, obj);
   }

   json_object_object_add(root, "devices", devices);
   json_object_object_add(root, "count", json_object_new_int(list->count));

   const char *json_str = json_object_to_json_string(root);
   int len = snprintf(buf, buf_size, "%s", json_str);

   json_object_put(root);
   return len;
}
