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
 * Home Assistant Service Implementation
 *
 * REST API client for Home Assistant with entity caching, area enrichment,
 * and domain-aware fuzzy matching. Uses per-request CURL handles for
 * thread safety.
 */

#include "tools/homeassistant_service.h"

#include <ctype.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/curl_buffer.h"
#include "core/str_fuzzy.h"
#include "logging.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Constants
 * ============================================================================= */
#define HA_MAX_RESPONSE_SIZE (2 * 1024 * 1024) /* 2MB for /api/states */
#define HA_SMALL_RESPONSE_SIZE (128 * 1024)    /* 128KB for other endpoints */
#define HA_API_RETRY_BASE_MS 500

/* =============================================================================
 * Domain Lookup Table (sorted for bsearch)
 * ============================================================================= */
typedef struct {
   const char *name;
   ha_domain_t domain;
} domain_map_entry_t;

static const domain_map_entry_t s_domain_map[] = {
   { "alarm_control_panel", HA_DOMAIN_ALARM },
   { "automation", HA_DOMAIN_AUTOMATION },
   { "binary_sensor", HA_DOMAIN_BINARY_SENSOR },
   { "climate", HA_DOMAIN_CLIMATE },
   { "cover", HA_DOMAIN_COVER },
   { "fan", HA_DOMAIN_FAN },
   { "input_boolean", HA_DOMAIN_INPUT_BOOLEAN },
   { "light", HA_DOMAIN_LIGHT },
   { "lock", HA_DOMAIN_LOCK },
   { "media_player", HA_DOMAIN_MEDIA_PLAYER },
   { "scene", HA_DOMAIN_SCENE },
   { "script", HA_DOMAIN_SCRIPT },
   { "sensor", HA_DOMAIN_SENSOR },
   { "switch", HA_DOMAIN_SWITCH },
   { "vacuum", HA_DOMAIN_VACUUM },
};
static const int s_domain_map_count = (int)(sizeof(s_domain_map) / sizeof(s_domain_map[0]));

/* =============================================================================
 * Area Cache
 * ============================================================================= */
#define HA_MAX_AREAS 128

typedef struct {
   char entity_id[HA_MAX_ENTITY_ID];
   char area_name[64];
} ha_area_entry_t;

typedef struct {
   ha_area_entry_t entries[HA_MAX_AREAS * 4]; /* Generous: many entities per area */
   int count;
   int64_t cached_at;
} ha_area_cache_t;

/* =============================================================================
 * Internal State
 * ============================================================================= */
static struct {
   bool initialized;
   bool connected;
   char base_url[512];
   char token[512];
   ha_entity_list_t entity_cache;
   ha_area_cache_t area_cache;
   pthread_rwlock_t rwlock;
   char version[32];
} s_ha = { .rwlock = PTHREAD_RWLOCK_INITIALIZER };

/* =============================================================================
 * Secure Memory Operations
 * ============================================================================= */
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

/* =============================================================================
 * Domain Lookup
 * ============================================================================= */
static int domain_compare(const void *a, const void *b) {
   const char *key = (const char *)a;
   const domain_map_entry_t *entry = (const domain_map_entry_t *)b;
   return strcmp(key, entry->name);
}

ha_domain_t homeassistant_parse_domain(const char *entity_id) {
   if (!entity_id)
      return HA_DOMAIN_UNKNOWN;

   const char *dot = strchr(entity_id, '.');
   if (!dot || dot == entity_id)
      return HA_DOMAIN_UNKNOWN;

   size_t domain_len = (size_t)(dot - entity_id);
   char domain_str[32];
   if (domain_len >= sizeof(domain_str))
      return HA_DOMAIN_UNKNOWN;

   memcpy(domain_str, entity_id, domain_len);
   domain_str[domain_len] = '\0';

   domain_map_entry_t *found = bsearch(domain_str, s_domain_map, s_domain_map_count,
                                       sizeof(domain_map_entry_t), domain_compare);
   return found ? found->domain : HA_DOMAIN_UNKNOWN;
}

const char *homeassistant_domain_str(ha_domain_t domain) {
   switch (domain) {
      case HA_DOMAIN_LIGHT:
         return "light";
      case HA_DOMAIN_SWITCH:
         return "switch";
      case HA_DOMAIN_CLIMATE:
         return "climate";
      case HA_DOMAIN_LOCK:
         return "lock";
      case HA_DOMAIN_COVER:
         return "cover";
      case HA_DOMAIN_MEDIA_PLAYER:
         return "media_player";
      case HA_DOMAIN_FAN:
         return "fan";
      case HA_DOMAIN_SCENE:
         return "scene";
      case HA_DOMAIN_SCRIPT:
         return "script";
      case HA_DOMAIN_AUTOMATION:
         return "automation";
      case HA_DOMAIN_SENSOR:
         return "sensor";
      case HA_DOMAIN_BINARY_SENSOR:
         return "binary_sensor";
      case HA_DOMAIN_INPUT_BOOLEAN:
         return "input_boolean";
      case HA_DOMAIN_VACUUM:
         return "vacuum";
      case HA_DOMAIN_ALARM:
         return "alarm_control_panel";
      default:
         return "unknown";
   }
}

static const char *const s_error_strings[] = {
   [HA_OK] = "OK",
   [HA_ERR_NOT_CONFIGURED] = "Home Assistant not configured",
   [HA_ERR_NOT_CONNECTED] = "Home Assistant not connected",
   [HA_ERR_NETWORK] = "Network error",
   [HA_ERR_API] = "API error",
   [HA_ERR_ENTITY_NOT_FOUND] =
       "Entity not found.  Call action='list' first to see the user's configured "
       "entities and match against their friendly_name or entity_id.",
   [HA_ERR_INVALID_PARAM] = "Invalid parameter",
   [HA_ERR_RATE_LIMITED] = "Rate limited",
   [HA_ERR_MEMORY] = "Memory allocation failed",
};

const char *homeassistant_error_str(ha_error_t err) {
   if (err >= 0 && (size_t)err < sizeof(s_error_strings) / sizeof(s_error_strings[0]) &&
       s_error_strings[err]) {
      return s_error_strings[err];
   }
   return "Unknown error";
}

/* =============================================================================
 * Entity ID Validation (prevents path traversal in service calls)
 * ============================================================================= */
static bool is_valid_entity_id(const char *id) {
   if (!id)
      return false;
   size_t len = strlen(id);
   if (len == 0 || len >= HA_MAX_ENTITY_ID)
      return false;

   bool has_dot = false;
   for (size_t i = 0; i < len; i++) {
      char c = id[i];
      if (c == '.') {
         has_dot = true;
      } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
         return false;
      }
   }
   return has_dot;
}

/* =============================================================================
 * Service name validation (prevents path traversal)
 * ============================================================================= */
static bool is_valid_service_name(const char *name) {
   if (!name)
      return false;
   size_t len = strlen(name);
   if (len == 0 || len > 64)
      return false;
   for (size_t i = 0; i < len; i++) {
      char c = name[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
         return false;
   }
   return true;
}

/* =============================================================================
 * CURL Request Helper (per-request handle, security hardened)
 * ============================================================================= */
static ha_error_t do_api_request(const char *method,
                                 const char *path,
                                 const char *post_data,
                                 curl_buffer_t *response,
                                 long *http_code_out) {
   /* Copy base_url and token under read lock to prevent races with cleanup */
   char local_base_url[512];
   char local_token[512];
   pthread_rwlock_rdlock(&s_ha.rwlock);
   if (!s_ha.initialized) {
      pthread_rwlock_unlock(&s_ha.rwlock);
      return HA_ERR_NOT_CONFIGURED;
   }
   safe_strscpy(local_base_url, s_ha.base_url);
   safe_strscpy(local_token, s_ha.token);
   pthread_rwlock_unlock(&s_ha.rwlock);

   CURL *curl = curl_easy_init();
   if (!curl) {
      secure_zero(local_token, sizeof(local_token));
      return HA_ERR_MEMORY;
   }

   /* Build URL */
   char url[1024];
   snprintf(url, sizeof(url), "%s%s", local_base_url, path);

   /* Build auth header — zero after use */
   char auth_header[600];
   snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", local_token);
   secure_zero(local_token, sizeof(local_token));

   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, auth_header);
   headers = curl_slist_append(headers, "Content-Type: application/json");

   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)HA_API_TIMEOUT_SEC);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L); /* No redirects (SSRF prevention) */
   DAWN_CURL_SET_PROTOCOLS(curl, "http,https");
   curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

   if (post_data) {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
      if (strcmp(method, "POST") == 0) {
         curl_easy_setopt(curl, CURLOPT_POST, 1L);
      }
   }

   ha_error_t result = HA_OK;
   CURLcode res = curl_easy_perform(curl);

   /* Zero auth header immediately after use */
   secure_zero(auth_header, sizeof(auth_header));

   if (res != CURLE_OK) {
      OLOG_ERROR("Home Assistant: CURL error: %s (url: %s)", curl_easy_strerror(res), path);
      result = HA_ERR_NETWORK;
   } else {
      long http_code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
      if (http_code_out)
         *http_code_out = http_code;

      if (http_code == 401) {
         OLOG_ERROR("Home Assistant: Authentication failed (401)");
         result = HA_ERR_NOT_CONNECTED;
         __atomic_store_n(&s_ha.connected, false, __ATOMIC_RELEASE);
      } else if (http_code == 429) {
         OLOG_WARNING("Home Assistant: Rate limited (429)");
         result = HA_ERR_RATE_LIMITED;
      } else if (http_code >= 400) {
         OLOG_ERROR("Home Assistant: API error %ld for %s", http_code, path);
         result = HA_ERR_API;
      }
   }

   curl_slist_free_all(headers);
   curl_easy_cleanup(curl);
   return result;
}

/* Retry wrapper */
static ha_error_t do_api_request_with_retry(const char *method,
                                            const char *path,
                                            const char *post_data,
                                            curl_buffer_t *response,
                                            long *http_code_out) {
   ha_error_t err;
   for (int attempt = 0; attempt < HA_API_MAX_RETRIES; attempt++) {
      curl_buffer_reset(response);
      err = do_api_request(method, path, post_data, response, http_code_out);

      if (err == HA_OK || err == HA_ERR_NOT_CONFIGURED || err == HA_ERR_NOT_CONNECTED ||
          err == HA_ERR_INVALID_PARAM) {
         return err;
      }

      /* Exponential backoff before retry */
      if (attempt < HA_API_MAX_RETRIES - 1) {
         int delay_ms = HA_API_RETRY_BASE_MS * (1 << attempt);
         struct timespec ts = { .tv_sec = delay_ms / 1000,
                                .tv_nsec = (delay_ms % 1000) * 1000000L };
         nanosleep(&ts, NULL);
      }
   }
   return err;
}

/* =============================================================================
 * Service Call Helper (validates inputs, builds JSON)
 * ============================================================================= */
static ha_error_t call_service_json(const char *domain,
                                    const char *service,
                                    const char *entity_id,
                                    json_object *data) {
   /* Ownership of 'data' is taken unconditionally: free it on every path,
    * including the validation-failure early returns below, so an untrusted
    * caller (e.g. the WebUI ha_call_service handler) cannot leak it. */
   if (!is_valid_service_name(domain) || !is_valid_service_name(service)) {
      OLOG_ERROR("Home Assistant: Invalid domain/service name");
      if (data)
         json_object_put(data);
      return HA_ERR_INVALID_PARAM;
   }
   if (entity_id && !is_valid_entity_id(entity_id)) {
      OLOG_ERROR("Home Assistant: Invalid entity_id format");
      if (data)
         json_object_put(data);
      return HA_ERR_INVALID_PARAM;
   }

   /* Build service data JSON.
    * Takes ownership of 'data' if non-NULL (will be freed by this function).
    * If 'data' is NULL, an empty object is created internally. */
   json_object *body = data ? data : json_object_new_object();
   if (!body) {
      return HA_ERR_MEMORY;
   }

   if (entity_id) {
      json_object_object_add(body, "entity_id", json_object_new_string(entity_id));
   }

   const char *json_str = json_object_to_json_string(body);

   char path[256];
   snprintf(path, sizeof(path), "/api/services/%s/%s", domain, service);

   curl_buffer_t response;
   curl_buffer_init(&response);

   long http_code = 0;
   ha_error_t err = do_api_request_with_retry("POST", path, json_str, &response, &http_code);

   curl_buffer_free(&response);
   json_object_put(body);

   if (err == HA_OK) {
      OLOG_INFO("Home Assistant: Called %s/%s on %s", domain, service,
                entity_id ? entity_id : "(no entity)");
   }

   return err;
}

/* Public generic service call — derives the domain from the entity_id prefix when
 * the caller omits it, then delegates to call_service_json (which validates and
 * takes ownership of 'data'). */
ha_error_t homeassistant_call_service(const char *domain,
                                      const char *service,
                                      const char *entity_id,
                                      json_object *data) {
   char derived[32];
   if ((!domain || !domain[0]) && entity_id) {
      const char *dot = strchr(entity_id, '.');
      if (dot && (size_t)(dot - entity_id) < sizeof(derived)) {
         memcpy(derived, entity_id, (size_t)(dot - entity_id));
         derived[dot - entity_id] = '\0';
         domain = derived;
      }
   }
   return call_service_json(domain, service, entity_id, data);
}

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */
ha_error_t homeassistant_init(const char *url, const char *token) {
   if (!url || !token || !url[0] || !token[0]) {
      return HA_ERR_INVALID_PARAM;
   }

   /* Validate URL scheme */
   if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
      OLOG_ERROR("Home Assistant: URL must start with http:// or https://");
      return HA_ERR_INVALID_PARAM;
   }

   pthread_rwlock_wrlock(&s_ha.rwlock);

   safe_strscpy(s_ha.base_url, url);

   /* Strip trailing slash */
   size_t url_len = strlen(s_ha.base_url);
   if (url_len > 0 && s_ha.base_url[url_len - 1] == '/') {
      s_ha.base_url[url_len - 1] = '\0';
   }

   safe_strscpy(s_ha.token, token);

   s_ha.initialized = true;
   s_ha.connected = false;
   s_ha.entity_cache.count = 0;
   s_ha.entity_cache.cached_at = 0;
   s_ha.area_cache.count = 0;
   s_ha.area_cache.cached_at = 0;
   s_ha.version[0] = '\0';

   pthread_rwlock_unlock(&s_ha.rwlock);

   /* Test connection */
   ha_error_t err = homeassistant_test_connection();
   if (err == HA_OK) {
      OLOG_INFO("Home Assistant: Initialized successfully (%s)", s_ha.base_url);
   } else {
      OLOG_WARNING("Home Assistant: Init succeeded but connection test failed: %s",
                   homeassistant_error_str(err));
   }

   return HA_OK; /* Init succeeded even if connection test failed */
}

void homeassistant_cleanup(void) {
   pthread_rwlock_wrlock(&s_ha.rwlock);
   s_ha.initialized = false;
   s_ha.connected = false;
   secure_zero(s_ha.token, sizeof(s_ha.token));
   s_ha.entity_cache.count = 0;
   s_ha.area_cache.count = 0;
   pthread_rwlock_unlock(&s_ha.rwlock);
   OLOG_INFO("Home Assistant: Cleaned up");
}

bool homeassistant_is_configured(void) {
   return s_ha.initialized;
}

bool homeassistant_is_connected(void) {
   return s_ha.initialized && __atomic_load_n(&s_ha.connected, __ATOMIC_ACQUIRE);
}

ha_error_t homeassistant_copy_credentials(char *url_out,
                                          size_t url_len,
                                          char *token_out,
                                          size_t token_len) {
   if (!url_out || !token_out || url_len == 0 || token_len == 0) {
      return HA_ERR_INVALID_PARAM;
   }
   pthread_rwlock_rdlock(&s_ha.rwlock);
   if (!s_ha.initialized || !s_ha.base_url[0] || !s_ha.token[0]) {
      pthread_rwlock_unlock(&s_ha.rwlock);
      url_out[0] = '\0';
      token_out[0] = '\0';
      return HA_ERR_NOT_CONFIGURED;
   }
   safe_strncpy(url_out, s_ha.base_url, url_len);
   safe_strncpy(token_out, s_ha.token, token_len);
   pthread_rwlock_unlock(&s_ha.rwlock);
   return HA_OK;
}

ha_error_t homeassistant_test_connection(void) {
   if (!s_ha.initialized)
      return HA_ERR_NOT_CONFIGURED;

   curl_buffer_t response;
   curl_buffer_init(&response);

   long http_code = 0;
   ha_error_t err = do_api_request("GET", "/api/", NULL, &response, &http_code);

   if (err == HA_OK && response.data) {
      json_object *root = json_tokener_parse(response.data);
      if (root) {
         json_object *msg_obj;
         if (json_object_object_get_ex(root, "message", &msg_obj)) {
            const char *msg = json_object_get_string(msg_obj);
            if (msg && strstr(msg, "API running")) {
               __atomic_store_n(&s_ha.connected, true, __ATOMIC_RELEASE);
            }
         }
         /* Try to extract version */
         json_object *ver_obj;
         if (json_object_object_get_ex(root, "version", &ver_obj)) {
            const char *ver = json_object_get_string(ver_obj);
            if (ver) {
               safe_strscpy(s_ha.version, ver);
            }
         }
         json_object_put(root);
      }
   }

   curl_buffer_free(&response);
   return __atomic_load_n(&s_ha.connected, __ATOMIC_ACQUIRE) ? HA_OK
                                                             : (err == HA_OK ? HA_ERR_API : err);
}

ha_error_t homeassistant_get_status(ha_status_t *status) {
   if (!status)
      return HA_ERR_INVALID_PARAM;

   status->configured = s_ha.initialized;
   status->connected = __atomic_load_n(&s_ha.connected, __ATOMIC_ACQUIRE);
   status->entity_count = s_ha.entity_cache.count;
   safe_strscpy(status->version, s_ha.version);
   safe_strscpy(status->url, s_ha.base_url);

   return HA_OK;
}

/* =============================================================================
 * Area Registry Enrichment
 * ============================================================================= */

/* Comparator for area cache entries (sorted by entity_id) */
static int area_entry_compare(const void *a, const void *b) {
   const ha_area_entry_t *ea = (const ha_area_entry_t *)a;
   const ha_area_entry_t *eb = (const ha_area_entry_t *)b;
   return strcmp(ea->entity_id, eb->entity_id);
}

/* Jinja2 template that produces a JSON array of {entity_id, area} objects.
 * Uses namespace() to accumulate results across nested loops, then to_json
 * for safe serialization (handles entity IDs with special characters).
 *
 * Unescaped Jinja2 (what HA actually evaluates):
 *
 *   {% set ns = namespace(items=[]) %}
 *   {% for a in areas() %}
 *     {% set aname = area_name(a) %}
 *     {% for e in area_entities(a) %}
 *       {% set ns.items = ns.items + [{'entity_id': e, 'area': aname}] %}
 *     {% endfor %}
 *   {% endfor %}
 *   {{ ns.items | to_json }}
 */
static const char HA_AREA_TEMPLATE[] =
    "{\"template\": \"{% set ns = namespace(items=[]) %}"
    "{% for a in areas() %}"
    "{% set aname = area_name(a) %}"
    "{% for e in area_entities(a) %}"
    "{% set ns.items = ns.items + [{'entity_id': e, 'area': aname}] %}"
    "{% endfor %}"
    "{% endfor %}"
    "{{ ns.items | to_json }}\"}";

/**
 * @brief Fetch area→entity mappings from HA (no lock held — does network I/O).
 *
 * Parses the result into @p out. Caller must hold the write lock when
 * committing @p out into s_ha.area_cache.
 *
 * @return true if new data was fetched, false if cache is still valid or fetch failed.
 */
static bool fetch_area_data(ha_area_cache_t *out) {
   int64_t now = (int64_t)time(NULL);
   if (s_ha.area_cache.cached_at > 0 && (now - s_ha.area_cache.cached_at) < HA_AREA_CACHE_TTL_SEC) {
      return false; /* Still valid */
   }

   /* The area/entity registries are WebSocket-only in HA — no REST endpoint.
    * Use POST /api/template with Jinja2 to query areas() and area_entities()
    * in a single call, returning entity→area mappings as JSON. */
   curl_buffer_t response;
   curl_buffer_init_with_max(&response, HA_MAX_RESPONSE_SIZE);
   long http_code = 0;

   ha_error_t err = do_api_request("POST", "/api/template", HA_AREA_TEMPLATE, &response,
                                   &http_code);
   if (err != HA_OK) {
      curl_buffer_free(&response);
      OLOG_INFO("Home Assistant: Area template query failed (will continue without areas)");
      out->count = 0;
      out->cached_at = now;
      return true;
   }

   /* Parse response: [{entity_id, area}, ...] */
   json_object *root = json_tokener_parse(response.data);
   curl_buffer_free(&response);
   if (!root || !json_object_is_type(root, json_type_array)) {
      if (root)
         json_object_put(root);
      OLOG_INFO("Home Assistant: Area template returned non-array (will continue without areas)");
      out->count = 0;
      out->cached_at = now;
      return true;
   }

   out->count = 0;
   int n = json_object_array_length(root);
   for (int i = 0; i < n && out->count < HA_MAX_AREAS * 4; i++) {
      json_object *item = json_object_array_get_idx(root, i);
      json_object *eid_obj, *area_obj;
      if (!json_object_object_get_ex(item, "entity_id", &eid_obj) ||
          !json_object_object_get_ex(item, "area", &area_obj))
         continue;

      const char *eid = json_object_get_string(eid_obj);
      const char *area = json_object_get_string(area_obj);
      if (!eid || !eid[0] || !area || !area[0])
         continue;

      ha_area_entry_t *entry = &out->entries[out->count];
      safe_strscpy(entry->entity_id, eid);
      safe_strscpy(entry->area_name, area);
      out->count++;
   }

   json_object_put(root);

   /* Sort area cache by entity_id for O(log n) lookup via bsearch */
   if (out->count > 1) {
      qsort(out->entries, out->count, sizeof(ha_area_entry_t), area_entry_compare);
   }

   out->cached_at = now;
   OLOG_INFO("Home Assistant: Cached %d entity-area assignments", out->count);
   return true;
}

int homeassistant_list_areas(char areas[][64], int max_areas) {
   if (!areas || max_areas <= 0)
      return 0;

   int count = 0;
   pthread_rwlock_rdlock(&s_ha.rwlock);

   for (int i = 0; i < s_ha.area_cache.count && count < max_areas; i++) {
      const char *name = s_ha.area_cache.entries[i].area_name;
      /* Deduplicate: check if we already have this area */
      bool dup = false;
      for (int j = 0; j < count; j++) {
         if (strcmp(areas[j], name) == 0) {
            dup = true;
            break;
         }
      }
      if (!dup) {
         safe_strscpy(areas[count], name);
         count++;
      }
   }

   pthread_rwlock_unlock(&s_ha.rwlock);
   return count;
}

/* Look up area name for entity via binary search */
static const char *find_area_for_entity(const char *entity_id) {
   if (s_ha.area_cache.count == 0)
      return NULL;

   ha_area_entry_t key;
   safe_strscpy(key.entity_id, entity_id);

   ha_area_entry_t *found = bsearch(&key, s_ha.area_cache.entries, s_ha.area_cache.count,
                                    sizeof(ha_area_entry_t), area_entry_compare);
   return found ? found->area_name : NULL;
}

/* =============================================================================
 * Attribute Parsing Helper
 * ============================================================================= */

/**
 * @brief Parse domain-specific attributes from a JSON attributes object into an entity
 *
 * Shared by both the entity cache refresh (fetch_entities) and single-entity
 * fetch (homeassistant_get_entity_state) to avoid duplicated parsing logic.
 */
static void parse_entity_attributes(json_object *attrs, ha_entity_t *ent) {
   json_object *val;
   if (json_object_object_get_ex(attrs, "brightness", &val))
      ent->brightness = json_object_get_int(val);
   if (json_object_object_get_ex(attrs, "color_temp", &val))
      ent->color_temp = json_object_get_int(val);
   if (json_object_object_get_ex(attrs, "rgb_color", &val) &&
       json_object_is_type(val, json_type_array) && json_object_array_length(val) == 3) {
      ent->rgb_color[0] = json_object_get_int(json_object_array_get_idx(val, 0));
      ent->rgb_color[1] = json_object_get_int(json_object_array_get_idx(val, 1));
      ent->rgb_color[2] = json_object_get_int(json_object_array_get_idx(val, 2));
   }
   if (json_object_object_get_ex(attrs, "hs_color", &val) &&
       json_object_is_type(val, json_type_array) && json_object_array_length(val) == 2) {
      ent->hs_color[0] = json_object_get_double(json_object_array_get_idx(val, 0));
      ent->hs_color[1] = json_object_get_double(json_object_array_get_idx(val, 1));
   }
   if (json_object_object_get_ex(attrs, "color_mode", &val)) {
      const char *cm = json_object_get_string(val);
      if (cm)
         safe_strscpy(ent->color_mode, cm);
   }
   if (json_object_object_get_ex(attrs, "current_temperature", &val))
      ent->temperature = json_object_get_double(val);
   if (json_object_object_get_ex(attrs, "temperature", &val))
      ent->target_temp = json_object_get_double(val);
   if (json_object_object_get_ex(attrs, "hvac_mode", &val)) {
      const char *hvac = json_object_get_string(val);
      if (hvac)
         safe_strscpy(ent->hvac_mode, hvac);
   }
   if (json_object_object_get_ex(attrs, "current_position", &val))
      ent->cover_position = json_object_get_int(val);
   if (json_object_object_get_ex(attrs, "percentage", &val))
      ent->fan_percentage = json_object_get_int(val);
   if (json_object_object_get_ex(attrs, "hvac_modes", &val) &&
       json_object_is_type(val, json_type_array)) {
      int n = json_object_array_length(val);
      ent->hvac_modes_count = 0;
      for (int i = 0; i < n && ent->hvac_modes_count < HA_MAX_HVAC_MODES; i++) {
         const char *m = json_object_get_string(json_object_array_get_idx(val, i));
         if (m && m[0]) {
            safe_strscpy(ent->hvac_modes[ent->hvac_modes_count], m);
            ent->hvac_modes_count++;
         }
      }
   }
   if (json_object_object_get_ex(attrs, "unit_of_measurement", &val)) {
      const char *u = json_object_get_string(val);
      if (u)
         safe_strscpy(ent->unit_of_measurement, u);
   }
   if (json_object_object_get_ex(attrs, "device_class", &val)) {
      const char *dc = json_object_get_string(val);
      if (dc)
         safe_strscpy(ent->device_class, dc);
   }
}

/* Parse one HA state object (an /api/states element, or a state_changed
 * new_state) into a caller-owned cache slot. Zeroes `out` first (so every fixed
 * string is NUL-terminated regardless of strncpy). Returns SUCCESS if the entity
 * has a known domain and was populated, FAILURE if it should be skipped (missing
 * entity_id, malformed, or an unsupported domain).
 *
 * LOCK CONTRACT: call with s_ha.rwlock held for WRITE — it reads the area cache
 * via find_area_for_entity() (no internal locking) and writes into `out`.
 * Shared by the REST fetch (fetch_entities) and the WS seed/apply paths. */
static bool parse_one_state(json_object *entity_obj, ha_entity_t *out) {
   if (!entity_obj || !out) {
      return false;
   }
   json_object *eid_obj;
   if (!json_object_object_get_ex(entity_obj, "entity_id", &eid_obj)) {
      return false;
   }
   const char *eid = json_object_get_string(eid_obj);
   if (!eid) {
      return false;
   }
   const char *dot = strchr(eid, '.');
   if (!dot) {
      return false;
   }
   size_t domain_len = (size_t)(dot - eid);
   char domain_str[32];
   if (domain_len >= sizeof(domain_str)) {
      return false;
   }
   memcpy(domain_str, eid, domain_len);
   domain_str[domain_len] = '\0';

   /* Filter: only keep known domains (bsearch on sorted list) */
   if (!bsearch(domain_str, s_domain_map, s_domain_map_count, sizeof(domain_map_entry_t),
                domain_compare)) {
      return false;
   }

   memset(out, 0, sizeof(*out));
   safe_strscpy(out->entity_id, eid);
   safe_strscpy(out->domain_str, domain_str);
   out->domain = homeassistant_parse_domain(eid);

   /* State */
   json_object *state_obj;
   if (json_object_object_get_ex(entity_obj, "state", &state_obj)) {
      const char *state = json_object_get_string(state_obj);
      if (state)
         safe_strscpy(out->state, state);
   }

   /* Attributes */
   json_object *attrs;
   if (json_object_object_get_ex(entity_obj, "attributes", &attrs)) {
      json_object *fname_obj;
      if (json_object_object_get_ex(attrs, "friendly_name", &fname_obj)) {
         const char *fname = json_object_get_string(fname_obj);
         if (fname) {
            safe_strscpy(out->friendly_name, fname);
            str_fuzzy_tolower(out->friendly_name_lower, fname, sizeof(out->friendly_name_lower));
         }
      }
      if (!out->friendly_name[0]) {
         /* Use entity_id after dot as fallback */
         safe_strscpy(out->friendly_name, dot + 1);
         str_fuzzy_tolower(out->friendly_name_lower, dot + 1, sizeof(out->friendly_name_lower));
      }
      parse_entity_attributes(attrs, out);
   }

   /* Area enrichment */
   const char *area = find_area_for_entity(eid);
   if (area) {
      safe_strscpy(out->area_name, area);
   }

   return true;
}

/* =============================================================================
 * Entity Discovery
 * ============================================================================= */
static ha_error_t fetch_entities(void) {
   curl_buffer_t response;
   curl_buffer_init_with_max(&response, HA_MAX_RESPONSE_SIZE);

   long http_code = 0;
   ha_error_t err = do_api_request_with_retry("GET", "/api/states", NULL, &response, &http_code);
   if (err != HA_OK) {
      curl_buffer_free(&response);
      return err;
   }

   json_object *root = json_tokener_parse(response.data);
   curl_buffer_free(&response);
   if (!root || !json_object_is_type(root, json_type_array)) {
      if (root)
         json_object_put(root);
      return HA_ERR_API;
   }

   /* Fetch area data outside the lock (does network I/O) */
   ha_area_cache_t area_update;
   bool area_updated = fetch_area_data(&area_update);

   pthread_rwlock_wrlock(&s_ha.rwlock);

   /* Commit area data under write lock */
   if (area_updated) {
      s_ha.area_cache = area_update;
   }
   s_ha.entity_cache.count = 0;

   int total = json_object_array_length(root);
   for (int i = 0; i < total && s_ha.entity_cache.count < HA_MAX_ENTITIES; i++) {
      json_object *entity_obj = json_object_array_get_idx(root, i);
      ha_entity_t *ent = &s_ha.entity_cache.entities[s_ha.entity_cache.count];
      if (parse_one_state(entity_obj, ent)) {
         s_ha.entity_cache.count++;
      }
   }

   s_ha.entity_cache.cached_at = (int64_t)time(NULL);
   pthread_rwlock_unlock(&s_ha.rwlock);

   json_object_put(root);

   if (s_ha.entity_cache.count >= HA_MAX_ENTITIES) {
      OLOG_WARNING("Home Assistant: Entity cache full (%d entities, max %d)",
                   s_ha.entity_cache.count, HA_MAX_ENTITIES);
   } else {
      OLOG_INFO("Home Assistant: Cached %d entities", s_ha.entity_cache.count);
   }

   return HA_OK;
}

ha_error_t homeassistant_cache_replace_from_states(struct json_object *states_array) {
   if (!states_array || !json_object_is_type((json_object *)states_array, json_type_array)) {
      return HA_ERR_INVALID_PARAM;
   }
   json_object *arr = (json_object *)states_array;

   pthread_rwlock_wrlock(&s_ha.rwlock);
   s_ha.entity_cache.count = 0;
   int total = json_object_array_length(arr);
   for (int i = 0; i < total && s_ha.entity_cache.count < HA_MAX_ENTITIES; i++) {
      json_object *entity_obj = json_object_array_get_idx(arr, i);
      ha_entity_t *ent = &s_ha.entity_cache.entities[s_ha.entity_cache.count];
      if (parse_one_state(entity_obj, ent)) {
         s_ha.entity_cache.count++;
      }
   }
   s_ha.entity_cache.cached_at = (int64_t)time(NULL);
   int count = s_ha.entity_cache.count;
   /* NOTE: the WS plane must NOT touch s_ha.connected (the REST command-plane
    * reachability flag). This only rebuilds the read cache. */
   pthread_rwlock_unlock(&s_ha.rwlock);

   OLOG_INFO("Home Assistant: WS-seeded %d entities", count);
   return HA_OK;
}

/* Linear lookups over the HA registries (home-scale sizes — dozens to low
 * hundreds — so O(n) scans are fine and avoid building temp hash maps). */
static const char *reg_area_name_for_id(json_object *area_reg, const char *area_id) {
   if (!area_id || !area_id[0])
      return NULL;
   int n = json_object_array_length(area_reg);
   for (int i = 0; i < n; i++) {
      json_object *a = json_object_array_get_idx(area_reg, i);
      json_object *id_obj, *name_obj;
      if (json_object_object_get_ex(a, "area_id", &id_obj) &&
          json_object_object_get_ex(a, "name", &name_obj)) {
         const char *id = json_object_get_string(id_obj);
         if (id && strcmp(id, area_id) == 0)
            return json_object_get_string(name_obj);
      }
   }
   return NULL;
}

static const char *reg_device_area_id(json_object *device_reg, const char *device_id) {
   if (!device_id || !device_id[0])
      return NULL;
   int n = json_object_array_length(device_reg);
   for (int i = 0; i < n; i++) {
      json_object *d = json_object_array_get_idx(device_reg, i);
      json_object *id_obj;
      if (json_object_object_get_ex(d, "id", &id_obj)) {
         const char *id = json_object_get_string(id_obj);
         if (id && strcmp(id, device_id) == 0) {
            json_object *aid_obj;
            if (json_object_object_get_ex(d, "area_id", &aid_obj) &&
                !json_object_is_type(aid_obj, json_type_null))
               return json_object_get_string(aid_obj);
            return NULL;
         }
      }
   }
   return NULL;
}

ha_error_t homeassistant_cache_replace_areas(struct json_object *area_reg_v,
                                             struct json_object *entity_reg_v,
                                             struct json_object *device_reg_v) {
   json_object *area_reg = (json_object *)area_reg_v;
   json_object *entity_reg = (json_object *)entity_reg_v;
   json_object *device_reg = (json_object *)device_reg_v;
   if (!area_reg || !entity_reg || !device_reg || !json_object_is_type(area_reg, json_type_array) ||
       !json_object_is_type(entity_reg, json_type_array) ||
       !json_object_is_type(device_reg, json_type_array)) {
      return HA_ERR_INVALID_PARAM;
   }

   /* Build the entity_id -> area_name join into a temp cache (no lock; pure CPU).
    * area_cache is large (~98 KB), so heap-allocate rather than stack. */
   ha_area_cache_t *tmp = malloc(sizeof(*tmp));
   if (!tmp) {
      return HA_ERR_MEMORY;
   }
   tmp->count = 0;

   int ne = json_object_array_length(entity_reg);
   for (int i = 0; i < ne && tmp->count < HA_MAX_AREAS * 4; i++) {
      json_object *e = json_object_array_get_idx(entity_reg, i);
      json_object *eid_obj;
      if (!json_object_object_get_ex(e, "entity_id", &eid_obj))
         continue;
      const char *eid = json_object_get_string(eid_obj);
      if (!eid || !eid[0])
         continue;

      /* Area: direct entity area_id, else inherited from the entity's device. */
      const char *area_id = NULL;
      json_object *aid_obj;
      if (json_object_object_get_ex(e, "area_id", &aid_obj) &&
          !json_object_is_type(aid_obj, json_type_null)) {
         area_id = json_object_get_string(aid_obj);
      }
      if (!area_id || !area_id[0]) {
         json_object *did_obj;
         if (json_object_object_get_ex(e, "device_id", &did_obj) &&
             !json_object_is_type(did_obj, json_type_null)) {
            area_id = reg_device_area_id(device_reg, json_object_get_string(did_obj));
         }
      }
      if (!area_id || !area_id[0])
         continue;

      const char *name = reg_area_name_for_id(area_reg, area_id);
      if (!name || !name[0])
         continue;

      ha_area_entry_t *entry = &tmp->entries[tmp->count];
      safe_strscpy(entry->entity_id, eid);
      safe_strscpy(entry->area_name, name);
      tmp->count++;
   }
   if (tmp->count > 1) {
      qsort(tmp->entries, tmp->count, sizeof(ha_area_entry_t), area_entry_compare);
   }
   tmp->cached_at = (int64_t)time(NULL);

   pthread_rwlock_wrlock(&s_ha.rwlock);
   s_ha.area_cache = *tmp;
   pthread_rwlock_unlock(&s_ha.rwlock);
   int count = tmp->count;
   free(tmp);

   OLOG_INFO("Home Assistant: WS registry — %d entity-area assignments", count);
   return HA_OK;
}

/* Apply one state_changed to the cache. Caller holds s_ha.rwlock for WRITE.
 * new_state == null (or NULL) removes the entity; otherwise upsert. Parses into
 * a temp first so a malformed/unknown-domain event never corrupts an existing
 * slot (parse_one_state memsets its output). */
static void cache_apply_one_locked(const char *entity_id, json_object *new_state) {
   int idx = -1;
   for (int i = 0; i < s_ha.entity_cache.count; i++) {
      if (strcasecmp(s_ha.entity_cache.entities[i].entity_id, entity_id) == 0) {
         idx = i;
         break;
      }
   }
   bool is_null = !new_state || json_object_is_type(new_state, json_type_null);
   if (is_null) {
      if (idx >= 0) {
         for (int i = idx; i < s_ha.entity_cache.count - 1; i++) {
            s_ha.entity_cache.entities[i] = s_ha.entity_cache.entities[i + 1];
         }
         s_ha.entity_cache.count--;
      }
      /* The matching area_cache row (if any) is left until the next reconnect's
       * registry rebuild — harmless, since nothing reads an area row for an
       * entity no longer in entity_cache. */
      return;
   }
   ha_entity_t tmp;
   if (!parse_one_state(new_state, &tmp)) {
      return; /* unknown domain / malformed — leave any existing slot intact */
   }
   if (idx >= 0) {
      s_ha.entity_cache.entities[idx] = tmp;
   } else if (s_ha.entity_cache.count < HA_MAX_ENTITIES) {
      s_ha.entity_cache.entities[s_ha.entity_cache.count++] = tmp;
   }
}

ha_error_t homeassistant_cache_apply_batch(struct json_object *dirty_map) {
   json_object *map = (json_object *)dirty_map;
   if (!map || !json_object_is_type(map, json_type_object)) {
      return HA_ERR_INVALID_PARAM;
   }
   pthread_rwlock_wrlock(&s_ha.rwlock);
   json_object_object_foreach(map, key, val) {
      cache_apply_one_locked(key, val);
   }
   s_ha.entity_cache.cached_at = (int64_t)time(NULL);
   pthread_rwlock_unlock(&s_ha.rwlock);
   return HA_OK;
}

bool homeassistant_copy_entity(const char *entity_id, ha_entity_t *out) {
   if (!entity_id || !out) {
      return false;
   }
   bool found = false;
   pthread_rwlock_rdlock(&s_ha.rwlock);
   for (int i = 0; i < s_ha.entity_cache.count; i++) {
      if (strcasecmp(s_ha.entity_cache.entities[i].entity_id, entity_id) == 0) {
         *out = s_ha.entity_cache.entities[i];
         found = true;
         break;
      }
   }
   pthread_rwlock_unlock(&s_ha.rwlock);
   return found;
}

ha_error_t homeassistant_list_entities(const ha_entity_list_t **list) {
   if (!list)
      return HA_ERR_INVALID_PARAM;
   if (!s_ha.initialized)
      return HA_ERR_NOT_CONFIGURED;
   if (!__atomic_load_n(&s_ha.connected, __ATOMIC_ACQUIRE))
      return HA_ERR_NOT_CONNECTED;

   int64_t now = (int64_t)time(NULL);
   if (s_ha.entity_cache.cached_at > 0 &&
       (now - s_ha.entity_cache.cached_at) < HA_ENTITY_CACHE_TTL_SEC) {
      *list = &s_ha.entity_cache;
      return HA_OK;
   }

   ha_error_t err = fetch_entities();
   if (err == HA_OK) {
      *list = &s_ha.entity_cache;
   }
   return err;
}

ha_error_t homeassistant_refresh_entities(const ha_entity_list_t **list) {
   if (!list)
      return HA_ERR_INVALID_PARAM;
   if (!s_ha.initialized)
      return HA_ERR_NOT_CONFIGURED;
   if (!__atomic_load_n(&s_ha.connected, __ATOMIC_ACQUIRE))
      return HA_ERR_NOT_CONNECTED;

   /* Force area cache refresh too */
   s_ha.area_cache.cached_at = 0;

   ha_error_t err = fetch_entities();
   if (err == HA_OK) {
      *list = &s_ha.entity_cache;
   }
   return err;
}

ha_error_t homeassistant_snapshot_entities(ha_entity_list_t *out, bool force_refresh) {
   if (!out)
      return HA_ERR_INVALID_PARAM;
   if (!s_ha.initialized)
      return HA_ERR_NOT_CONFIGURED;
   if (!__atomic_load_n(&s_ha.connected, __ATOMIC_ACQUIRE))
      return HA_ERR_NOT_CONNECTED;

   /* Bring the cache up to date OUTSIDE the read lock (fetch_entities takes the
    * write lock internally).  Entity-only: unlike homeassistant_refresh_entities
    * we do NOT zero the area cache, so a control-reconcile / board poll re-polls
    * /api/states without a wasted /api/template area round-trip — areas keep
    * their own 1h TTL and still refresh via fetch_area_data when it expires. */
   if (force_refresh) {
      ha_error_t err = fetch_entities();
      if (err != HA_OK)
         return err;
   } else {
      int64_t now = (int64_t)time(NULL);
      if (!(s_ha.entity_cache.cached_at > 0 &&
            (now - s_ha.entity_cache.cached_at) < HA_ENTITY_CACHE_TTL_SEC)) {
         ha_error_t err = fetch_entities();
         if (err != HA_OK)
            return err;
      }
   }

   /* Copy the cache into caller memory under the read lock so the caller never
    * serializes the live struct while fetch_entities rewrites it in place. */
   pthread_rwlock_rdlock(&s_ha.rwlock);
   *out = s_ha.entity_cache;
   pthread_rwlock_unlock(&s_ha.rwlock);
   return HA_OK;
}

ha_error_t homeassistant_find_entity(const char *name,
                                     ha_domain_t domain_hint,
                                     const ha_entity_t **entity) {
   if (!name || !entity)
      return HA_ERR_INVALID_PARAM;

   /* Direct entity_id lookup if input contains '.' */
   if (strchr(name, '.')) {
      const ha_entity_list_t *list;
      ha_error_t err = homeassistant_list_entities(&list);
      if (err != HA_OK)
         return err;

      pthread_rwlock_rdlock(&s_ha.rwlock);
      for (int i = 0; i < list->count; i++) {
         if (strcasecmp(list->entities[i].entity_id, name) == 0) {
            *entity = &list->entities[i];
            pthread_rwlock_unlock(&s_ha.rwlock);
            return HA_OK;
         }
      }
      pthread_rwlock_unlock(&s_ha.rwlock);
      return HA_ERR_ENTITY_NOT_FOUND;
   }

   const ha_entity_list_t *list;
   ha_error_t err = homeassistant_list_entities(&list);
   if (err != HA_OK)
      return err;

   char needle_lower[256];
   str_fuzzy_tolower(needle_lower, name, sizeof(needle_lower));

   int best_score = 0;
   const ha_entity_t *best_match = NULL;

   pthread_rwlock_rdlock(&s_ha.rwlock);
   for (int i = 0; i < list->count; i++) {
      const ha_entity_t *ent = &list->entities[i];

      /* Domain filtering */
      if (domain_hint != HA_DOMAIN_UNKNOWN && ent->domain != domain_hint)
         continue;

      /* Score against friendly_name (pre-lowered) and entity_id */
      int score = str_fuzzy_score(ent->friendly_name_lower, needle_lower);

      char eid_lower[HA_MAX_ENTITY_ID];
      str_fuzzy_tolower(eid_lower, ent->entity_id, sizeof(eid_lower));
      int eid_score = str_fuzzy_score(eid_lower, needle_lower);
      if (eid_score > score)
         score = eid_score;

      if (score > best_score) {
         best_score = score;
         best_match = ent;
      }
   }
   pthread_rwlock_unlock(&s_ha.rwlock);

   if (best_score < 40 || !best_match) {
      OLOG_WARNING("Home Assistant: No entity matching '%s' (domain: %s, best score: %d)", name,
                   homeassistant_domain_str(domain_hint), best_score);
      return HA_ERR_ENTITY_NOT_FOUND;
   }

   OLOG_INFO("Home Assistant: Matched '%s' → '%s' (%s, score: %d)", name, best_match->friendly_name,
             best_match->entity_id, best_score);
   *entity = best_match;
   return HA_OK;
}

ha_error_t homeassistant_get_entity_state(const char *entity_id, ha_entity_t *out) {
   if (!entity_id || !out)
      return HA_ERR_INVALID_PARAM;
   if (!is_valid_entity_id(entity_id))
      return HA_ERR_INVALID_PARAM;
   if (!s_ha.initialized)
      return HA_ERR_NOT_CONFIGURED;

   char path[256];
   snprintf(path, sizeof(path), "/api/states/%s", entity_id);

   curl_buffer_t response;
   curl_buffer_init(&response);
   long http_code = 0;

   ha_error_t err = do_api_request_with_retry("GET", path, NULL, &response, &http_code);
   if (err != HA_OK) {
      curl_buffer_free(&response);
      return err;
   }

   if (http_code == 404) {
      curl_buffer_free(&response);
      return HA_ERR_ENTITY_NOT_FOUND;
   }

   json_object *root = json_tokener_parse(response.data);
   curl_buffer_free(&response);
   if (!root) {
      return HA_ERR_API;
   }

   memset(out, 0, sizeof(*out));
   safe_strscpy(out->entity_id, entity_id);
   out->domain = homeassistant_parse_domain(entity_id);

   json_object *state_obj;
   if (json_object_object_get_ex(root, "state", &state_obj)) {
      const char *state = json_object_get_string(state_obj);
      if (state)
         safe_strscpy(out->state, state);
   }

   json_object *attrs;
   if (json_object_object_get_ex(root, "attributes", &attrs)) {
      json_object *val;
      if (json_object_object_get_ex(attrs, "friendly_name", &val)) {
         const char *fname = json_object_get_string(val);
         if (fname)
            safe_strscpy(out->friendly_name, fname);
      }
      parse_entity_attributes(attrs, out);
   }

   json_object_put(root);
   return HA_OK;
}

/* =============================================================================
 * Device Control Functions
 * ============================================================================= */
ha_error_t homeassistant_turn_on(const char *entity_id) {
   ha_domain_t domain = homeassistant_parse_domain(entity_id);
   const char *domain_str = homeassistant_domain_str(domain);
   return call_service_json(domain_str, "turn_on", entity_id, NULL);
}

ha_error_t homeassistant_turn_off(const char *entity_id) {
   ha_domain_t domain = homeassistant_parse_domain(entity_id);
   const char *domain_str = homeassistant_domain_str(domain);
   return call_service_json(domain_str, "turn_off", entity_id, NULL);
}

ha_error_t homeassistant_toggle(const char *entity_id) {
   ha_domain_t domain = homeassistant_parse_domain(entity_id);
   const char *domain_str = homeassistant_domain_str(domain);
   return call_service_json(domain_str, "toggle", entity_id, NULL);
}

ha_error_t homeassistant_set_brightness(const char *entity_id, int pct) {
   if (pct < 0 || pct > 100)
      return HA_ERR_INVALID_PARAM;

   json_object *data = json_object_new_object();
   /* HA brightness is 0-255, convert from percentage */
   json_object_object_add(data, "brightness", json_object_new_int(pct * 255 / 100));
   return call_service_json("light", "turn_on", entity_id, data);
}

void homeassistant_rgb_to_hs(int r, int g, int b, double *out_hue, double *out_sat) {
   double rf = r / 255.0, gf = g / 255.0, bf = b / 255.0;
   double cmax = rf > gf ? (rf > bf ? rf : bf) : (gf > bf ? gf : bf);
   double cmin = rf < gf ? (rf < bf ? rf : bf) : (gf < bf ? gf : bf);
   double delta = cmax - cmin;

   double hue = 0.0;
   if (delta > 0.0001) {
      /* Use integer comparison for max-channel to avoid float equality fragility */
      int max_ch = (r >= g) ? (r >= b ? 0 : 2) : (g >= b ? 1 : 2);
      if (max_ch == 0)
         hue = 60.0 * fmod((gf - bf) / delta + 6.0, 6.0);
      else if (max_ch == 1)
         hue = 60.0 * ((bf - rf) / delta + 2.0);
      else
         hue = 60.0 * ((rf - gf) / delta + 4.0);
   }
   *out_hue = hue;
   *out_sat = (cmax > 0.0001) ? (delta / cmax) * 100.0 : 0.0;
}

ha_error_t homeassistant_set_color(const char *entity_id, int r, int g, int b) {
   if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255)
      return HA_ERR_INVALID_PARAM;

   double hue, sat;
   homeassistant_rgb_to_hs(r, g, b, &hue, &sat);
   return homeassistant_set_hs_color(entity_id, hue, sat);
}

ha_error_t homeassistant_set_hs_color(const char *entity_id, double hue, double saturation) {
   if (hue < 0.0 || hue >= 360.0 || saturation < 0.0 || saturation > 100.0)
      return HA_ERR_INVALID_PARAM;

   json_object *data = json_object_new_object();
   json_object *color = json_object_new_array();
   json_object_array_add(color, json_object_new_double(hue));
   json_object_array_add(color, json_object_new_double(saturation));
   json_object_object_add(data, "hs_color", color);
   return call_service_json("light", "turn_on", entity_id, data);
}

ha_error_t homeassistant_set_color_temp(const char *entity_id, int kelvin) {
   if (kelvin < 1000 || kelvin > 12000)
      return HA_ERR_INVALID_PARAM;

   json_object *data = json_object_new_object();
   /* Convert kelvin to mireds: mireds = 1000000 / kelvin */
   int mireds = 1000000 / kelvin;
   json_object_object_add(data, "color_temp", json_object_new_int(mireds));
   return call_service_json("light", "turn_on", entity_id, data);
}

ha_error_t homeassistant_set_temperature(const char *entity_id, double temp_f) {
   if (temp_f < 40 || temp_f > 100)
      return HA_ERR_INVALID_PARAM;

   json_object *data = json_object_new_object();
   json_object_object_add(data, "temperature", json_object_new_double(temp_f));
   return call_service_json("climate", "set_temperature", entity_id, data);
}

ha_error_t homeassistant_lock(const char *entity_id) {
   return call_service_json("lock", "lock", entity_id, NULL);
}

ha_error_t homeassistant_unlock(const char *entity_id) {
   return call_service_json("lock", "unlock", entity_id, NULL);
}

ha_error_t homeassistant_open_cover(const char *entity_id) {
   return call_service_json("cover", "open_cover", entity_id, NULL);
}

ha_error_t homeassistant_close_cover(const char *entity_id) {
   return call_service_json("cover", "close_cover", entity_id, NULL);
}

ha_error_t homeassistant_activate_scene(const char *entity_id) {
   return call_service_json("scene", "turn_on", entity_id, NULL);
}

ha_error_t homeassistant_run_script(const char *entity_id) {
   return call_service_json("script", "turn_on", entity_id, NULL);
}

ha_error_t homeassistant_trigger_automation(const char *entity_id) {
   return call_service_json("automation", "trigger", entity_id, NULL);
}
