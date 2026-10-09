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
 * Tool Registry Implementation
 *
 * Provides registration, lookup, and management for standalone tools.
 * Each tool registers its metadata, callback, and config parser.
 */

#include "tools/tool_registry.h"

#include <json-c/json.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config/dawn_config.h"
#include "core/device_types.h"
#include "dawn_error.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "toml.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Shared tool-argument helpers
 * ============================================================================= */

struct json_object *tool_parse_details(const char *value, bool no_required_fields) {
   if (value == NULL || value[0] == '\0') {
      return json_object_new_object();
   }
   struct json_object *parsed = json_tokener_parse(value);
   if (parsed != NULL) {
      if (json_object_is_type(parsed, json_type_object))
         return parsed;
      /* Valid JSON but not an object (array/string/number/bool/null): callers
       * read named fields via json_object_object_get, so a value like `[]`
       * would be treated as an empty object and silently drop a requested
       * filter.  Release it and fall through to the same handling as non-JSON. */
      json_object_put(parsed);
   }
   /* Not a JSON object.  A reasoning model often fills `details` with a prose
    * rationale instead of an args object; for an action that reads no required
    * field that is harmless, so degrade to an empty object.  For an action with
    * real fields a non-object value would silently drop the caller's request, so
    * return NULL and let the caller raise "invalid JSON in details parameter". */
   return no_required_fields ? json_object_new_object() : NULL;
}

/* =============================================================================
 * Module State
 * ============================================================================= */

/**
 * @brief Internal tool entry with runtime state
 */
typedef struct {
   tool_metadata_t metadata; /* Copy of registered metadata */
   bool enabled;             /* Runtime enable/disable */
   bool initialized;         /* init() has been called */
   bool registered;          /* Slot is in use */
} tool_entry_t;

static tool_entry_t s_tools[TOOL_MAX_REGISTERED];
static int s_tool_count = 0;
static bool s_initialized = false;
static bool s_locked = false;
static bool s_available = false;  /* True if init succeeded, false for degraded mode */
static bool s_cache_valid = true; /* Schema cache validity */
/* Rises on every change that can change a tool's schema or the set of tools
 * (tool_registry_generation). */
static _Atomic uint64_t s_generation;
static pthread_mutex_t s_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * Dynamic Enum Override Storage
 * ============================================================================= */

/**
 * @brief Storage for dynamically updated enum values
 *
 * When tool_registry_update_param_enum() is called, enum values are copied
 * here and the tool's param pointer is updated to point to this storage.
 */
typedef struct {
   int tool_index;                                  /* Index into s_tools */
   int param_index;                                 /* Index into tool's params */
   char values[TOOL_PARAM_ENUM_MAX][TOOL_NAME_MAX]; /* Deep copy of enum strings */
   const char *value_ptrs[TOOL_PARAM_ENUM_MAX];     /* Pointers for param struct */
   int count;                                       /* Number of values */
   bool active;                                     /* Slot in use */
   treg_param_t override_param;                     /* Copy of param with updated enum */
} enum_override_t;

/*
 * Enum override storage for runtime-discovered values (e.g., HUD scene names).
 * Memory footprint: 32 slots × ~1.5KB each = ~49KB static allocation.
 * This is acceptable for Jetson/embedded Linux targets with 4GB+ RAM.
 * If memory-constrained, reduce MAX_ENUM_OVERRIDES or use dynamic allocation.
 */
#define MAX_ENUM_OVERRIDES 32
static enum_override_t s_enum_overrides[MAX_ENUM_OVERRIDES];
static int s_override_count = 0;

/* Forward declarations */
static const treg_param_t *get_effective_param(int tool_index, int param_index);
static int count_device_type_patterns(tool_device_type_t device_type);

/* Alias lookup table */
#define MAX_ALIASES_TOTAL 256
typedef struct {
   char alias[TOOL_NAME_MAX];
   int tool_index; /* Index into s_tools */
} alias_entry_t;

static alias_entry_t s_aliases[MAX_ALIASES_TOTAL];
static int s_alias_count = 0;

/* =============================================================================
 * Hash Table for O(1) Lookups
 * ============================================================================= */

#define HASH_BUCKETS 128 /* Power of 2 for fast modulo */

typedef struct {
   char key[TOOL_NAME_MAX];
   int value; /* Index into s_tools, or -1 if empty */
} hash_entry_t;

static hash_entry_t s_name_hash[HASH_BUCKETS];
static hash_entry_t s_device_hash[HASH_BUCKETS];    /* device_string → tool index */
static hash_entry_t s_alias_hash[HASH_BUCKETS * 2]; /* More aliases than tools */

/**
 * @brief FNV-1a hash function for strings
 */
static uint32_t fnv1a_hash(const char *str) {
   uint32_t hash = 2166136261u;
   while (*str) {
      hash ^= (uint8_t)*str++;
      hash *= 16777619u;
   }
   return hash;
}

/**
 * @brief Insert into hash table with open addressing
 */
static void hash_insert(hash_entry_t *table, size_t size, const char *key, int value) {
   uint32_t idx = fnv1a_hash(key) % size;
   for (size_t i = 0; i < size; i++) {
      size_t probe = (idx + i) % size;
      if (table[probe].value == -1 || strcmp(table[probe].key, key) == 0) {
         safe_strncpy(table[probe].key, key, TOOL_NAME_MAX);
         table[probe].value = value;
         return;
      }
   }
   OLOG_WARNING("tool_registry: Hash table full, could not insert '%s'", key);
}

/**
 * @brief Lookup in hash table with open addressing
 * @return Index into s_tools, or -1 if not found
 */
static int hash_lookup(const hash_entry_t *table, size_t size, const char *key) {
   uint32_t idx = fnv1a_hash(key) % size;
   for (size_t i = 0; i < size; i++) {
      size_t probe = (idx + i) % size;
      if (table[probe].value == -1) {
         return -1; /* Empty slot = not found */
      }
      if (strcmp(table[probe].key, key) == 0) {
         return table[probe].value;
      }
   }
   return -1;
}

/**
 * @brief Initialize hash tables (mark all slots empty)
 */
static void hash_init(void) {
   for (size_t i = 0; i < HASH_BUCKETS; i++) {
      s_name_hash[i].value = -1;
      s_device_hash[i].value = -1;
   }
   for (size_t i = 0; i < HASH_BUCKETS * 2; i++) {
      s_alias_hash[i].value = -1;
   }
}

/* =============================================================================
 * Validation Helpers
 * ============================================================================= */

/**
 * @brief Validate dangerous tool has proper config
 *
 * TOOL_CAP_DANGEROUS tools must have:
 * - A config struct
 * - A config parser
 */
static bool validate_dangerous_tool(const tool_metadata_t *metadata) {
   if (!(metadata->capabilities & TOOL_CAP_DANGEROUS)) {
      return true; /* Not dangerous, no special requirements */
   }

   if (!metadata->config || !metadata->config_parser) {
      OLOG_ERROR("tool_registry: Dangerous tool '%s' must have config and parser", metadata->name);
      return false;
   }

   return true;
}

/**
 * @brief Validate tool's secret requirements match capabilities
 *
 * If tool declares secret_requirements, it must have TOOL_CAP_SECRETS.
 */
static bool validate_secret_requirements(const tool_metadata_t *metadata) {
   if (!metadata->secret_requirements) {
      return true; /* No secrets required */
   }

   /* Check if any secrets are declared */
   bool has_secrets = false;
   for (const tool_secret_requirement_t *req = metadata->secret_requirements; req->secret_name;
        req++) {
      has_secrets = true;
      break;
   }

   if (has_secrets && !(metadata->capabilities & TOOL_CAP_SECRETS)) {
      OLOG_ERROR("tool_registry: Tool '%s' requires secrets but lacks TOOL_CAP_SECRETS",
                 metadata->name);
      return false;
   }

   return true;
}

/**
 * @brief Check if a tool declared a specific secret
 */
static bool tool_declared_secret(const tool_metadata_t *metadata, const char *secret_name) {
   if (!metadata->secret_requirements) {
      return false;
   }

   for (const tool_secret_requirement_t *req = metadata->secret_requirements; req->secret_name;
        req++) {
      if (strcmp(req->secret_name, secret_name) == 0) {
         return true;
      }
   }
   return false;
}

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

int tool_registry_init(void) {
   pthread_mutex_lock(&s_registry_mutex);

   if (s_initialized) {
      pthread_mutex_unlock(&s_registry_mutex);
      return SUCCESS; /* Already initialized */
   }

   /* Clear state */
   memset(s_tools, 0, sizeof(s_tools));
   memset(s_aliases, 0, sizeof(s_aliases));
   memset(s_enum_overrides, 0, sizeof(s_enum_overrides));
   s_tool_count = 0;
   s_alias_count = 0;
   s_override_count = 0;
   s_locked = false;
   s_cache_valid = true;
   atomic_fetch_add(&s_generation, 1);

   /* Initialize hash tables */
   hash_init();

   s_initialized = true;
   s_available = true;
   OLOG_INFO("Tool registry initialized");

   pthread_mutex_unlock(&s_registry_mutex);
   return SUCCESS;
}

bool tool_registry_is_available(void) {
   return s_available;
}

int tool_registry_init_tools(void) {
   pthread_mutex_lock(&s_registry_mutex);

   if (!s_initialized) {
      OLOG_ERROR("tool_registry: Cannot init tools - registry not initialized");
      pthread_mutex_unlock(&s_registry_mutex);
      return 1;
   }

   int failures = 0;
   for (int i = 0; i < s_tool_count; i++) {
      tool_entry_t *entry = &s_tools[i];
      if (!entry->registered || entry->initialized) {
         continue;
      }

      if (entry->metadata.init) {
         OLOG_INFO("Initializing tool: %s", entry->metadata.name);
         int rc = entry->metadata.init();
         if (rc != 0) {
            OLOG_ERROR("tool_registry: Failed to initialize tool '%s': %d", entry->metadata.name,
                       rc);
            failures++;
         } else {
            entry->initialized = true;
         }
      } else {
         entry->initialized = true; /* No init function = always initialized */
      }
   }

   /* Log summary stats (calculate while still holding lock) */
   int total_variations = 0;
   for (int i = 0; i < s_tool_count; i++) {
      if (!s_tools[i].registered) {
         continue;
      }
      const tool_metadata_t *meta = &s_tools[i].metadata;
      int patterns = count_device_type_patterns(meta->device_type);
      int name_variations = 1 + meta->alias_count;
      total_variations += patterns * name_variations;
   }

   OLOG_INFO("tool_registry: %d tools initialized, %d direct command variations", s_tool_count,
             total_variations);

   pthread_mutex_unlock(&s_registry_mutex);
   return failures;
}

void tool_registry_lock(void) {
   pthread_mutex_lock(&s_registry_mutex);
   s_locked = true;
   OLOG_INFO("Tool registry locked - no further registrations allowed");
   pthread_mutex_unlock(&s_registry_mutex);
}

bool tool_registry_is_locked(void) {
   pthread_mutex_lock(&s_registry_mutex);
   bool locked = s_locked;
   pthread_mutex_unlock(&s_registry_mutex);
   return locked;
}

void tool_registry_shutdown(void) {
   pthread_mutex_lock(&s_registry_mutex);

   if (!s_initialized) {
      pthread_mutex_unlock(&s_registry_mutex);
      return;
   }

   /* Call cleanup in reverse order */
   for (int i = s_tool_count - 1; i >= 0; i--) {
      tool_entry_t *entry = &s_tools[i];
      if (entry->registered && entry->initialized && entry->metadata.cleanup) {
         OLOG_INFO("Cleaning up tool: %s", entry->metadata.name);
         entry->metadata.cleanup();
         entry->initialized = false;
      }
   }

   /* Clear state */
   memset(s_tools, 0, sizeof(s_tools));
   memset(s_aliases, 0, sizeof(s_aliases));
   memset(s_enum_overrides, 0, sizeof(s_enum_overrides));
   s_tool_count = 0;
   s_alias_count = 0;
   s_override_count = 0;
   s_locked = false;
   s_cache_valid = true;
   atomic_fetch_add(&s_generation, 1);
   s_initialized = false;

   hash_init();

   OLOG_INFO("Tool registry shutdown complete");

   pthread_mutex_unlock(&s_registry_mutex);
}

/* =============================================================================
 * Registration Functions
 * ============================================================================= */

int tool_registry_register(const tool_metadata_t *metadata) {
   if (!metadata || !metadata->name || !metadata->callback) {
      OLOG_ERROR("tool_registry: Invalid registration - missing name or callback");
      return 1;
   }

   pthread_mutex_lock(&s_registry_mutex);

   if (!s_initialized) {
      OLOG_ERROR("tool_registry: Cannot register - registry not initialized");
      pthread_mutex_unlock(&s_registry_mutex);
      return 1;
   }

   if (s_locked) {
      OLOG_ERROR("tool_registry: Cannot register '%s' - registry is locked", metadata->name);
      pthread_mutex_unlock(&s_registry_mutex);
      return 1;
   }

   if (s_tool_count >= TOOL_MAX_REGISTERED) {
      OLOG_ERROR("tool_registry: Maximum tool count (%d) reached", TOOL_MAX_REGISTERED);
      pthread_mutex_unlock(&s_registry_mutex);
      return 1;
   }

   /* Check for duplicate name */
   if (hash_lookup(s_name_hash, HASH_BUCKETS, metadata->name) >= 0) {
      OLOG_ERROR("tool_registry: Tool '%s' already registered", metadata->name);
      pthread_mutex_unlock(&s_registry_mutex);
      return 1;
   }

   /* Validate dangerous tool requirements */
   if (!validate_dangerous_tool(metadata)) {
      pthread_mutex_unlock(&s_registry_mutex);
      return 1;
   }

   /* Validate secret requirements */
   if (!validate_secret_requirements(metadata)) {
      pthread_mutex_unlock(&s_registry_mutex);
      return 1;
   }

   /* Its kinds of action name real actions */
   char why[160];
   if (tool_action_kinds_validate(metadata, why, sizeof(why)) != SUCCESS) {
      OLOG_ERROR("tool_registry: Tool '%s' action kinds: %s", metadata->name, why);
      pthread_mutex_unlock(&s_registry_mutex);
      return 1;
   }

   /* Register the tool */
   int idx = s_tool_count;
   tool_entry_t *entry = &s_tools[idx];

   /* Copy metadata (shallow copy - pointers remain valid as they point to static data) */
   entry->metadata = *metadata;
   entry->registered = true;
   entry->initialized = false;

   /* Default enabled state — all tools start enabled, including dangerous ones.
    * TOOL_CAP_DANGEROUS controls LLM confirmation behavior, not availability.
    * Tools can be explicitly disabled via their config section. */
   entry->enabled = true;

   /* Insert into name hash table */
   hash_insert(s_name_hash, HASH_BUCKETS, metadata->name, idx);

   /* Insert device_string into hash table (if different from name) */
   if (metadata->device_string && metadata->device_string[0] != '\0' &&
       strcmp(metadata->device_string, metadata->name) != 0) {
      hash_insert(s_device_hash, HASH_BUCKETS, metadata->device_string, idx);
   }

   /* Register aliases */
   for (int i = 0; i < metadata->alias_count && i < TOOL_ALIAS_MAX; i++) {
      if (metadata->aliases[i] && s_alias_count < MAX_ALIASES_TOTAL) {
         safe_strncpy(s_aliases[s_alias_count].alias, metadata->aliases[i], TOOL_NAME_MAX);
         s_aliases[s_alias_count].tool_index = idx;
         hash_insert(s_alias_hash, HASH_BUCKETS * 2, metadata->aliases[i], idx);
         s_alias_count++;
      }
   }

   s_tool_count++;
   OLOG_INFO("Registered tool: %s (caps=0x%x)", metadata->name, metadata->capabilities);

   atomic_fetch_add(&s_generation, 1); /* after the change, never before */
   pthread_mutex_unlock(&s_registry_mutex);
   return SUCCESS;
}

/* =============================================================================
 * Lookup Functions
 * ============================================================================= */

const tool_metadata_t *tool_registry_lookup(const char *name) {
   if (!name || !s_initialized) {
      return NULL;
   }

   int idx = hash_lookup(s_name_hash, HASH_BUCKETS, name);
   if (idx >= 0 && s_tools[idx].registered) {
      return &s_tools[idx].metadata;
   }
   return NULL;
}

const tool_metadata_t *tool_registry_lookup_alias(const char *alias) {
   if (!alias || !s_initialized) {
      return NULL;
   }

   int idx = hash_lookup(s_alias_hash, HASH_BUCKETS * 2, alias);
   if (idx >= 0 && s_tools[idx].registered) {
      return &s_tools[idx].metadata;
   }
   return NULL;
}

/**
 * @brief Look up a tool by device_string
 *
 * Uses hash table for O(1) lookup.
 */
static const tool_metadata_t *tool_registry_lookup_device_string(const char *device_string) {
   if (!device_string || !s_initialized) {
      return NULL;
   }

   int idx = hash_lookup(s_device_hash, HASH_BUCKETS, device_string);
   if (idx >= 0 && s_tools[idx].registered) {
      return &s_tools[idx].metadata;
   }
   return NULL;
}

const tool_metadata_t *tool_registry_find(const char *name_or_alias) {
   if (!name_or_alias || !s_initialized) {
      return NULL;
   }

   /* Try name first */
   const tool_metadata_t *meta = tool_registry_lookup(name_or_alias);
   if (meta) {
      return meta;
   }

   /* Try device_string second (takes precedence over aliases) */
   meta = tool_registry_lookup_device_string(name_or_alias);
   if (meta) {
      return meta;
   }

   /* Try alias last */
   return tool_registry_lookup_alias(name_or_alias);
}

tool_callback_fn tool_registry_get_callback(const char *name) {
   const tool_metadata_t *meta = tool_registry_find(name);
   return meta ? meta->callback : NULL;
}

bool tool_registry_is_enabled(const char *name) {
   if (!name || !s_initialized) {
      return false;
   }

   int idx = hash_lookup(s_name_hash, HASH_BUCKETS, name);
   if (idx < 0) {
      idx = hash_lookup(s_alias_hash, HASH_BUCKETS * 2, name);
   }

   if (idx >= 0 && s_tools[idx].registered) {
      return s_tools[idx].enabled;
   }
   return false;
}

int tool_registry_validate_schedulable(const char *tool_name,
                                       const char *tool_action,
                                       const char *tool_value,
                                       char *err_buf,
                                       size_t err_buf_size) {
   if (!tool_name || !tool_name[0]) {
      if (err_buf && err_buf_size)
         snprintf(err_buf, err_buf_size, "tool_name is required");
      return FAILURE;
   }
   const tool_metadata_t *meta = tool_registry_find(tool_name);
   if (!meta) {
      if (err_buf && err_buf_size)
         snprintf(err_buf, err_buf_size, "unknown tool '%s'", tool_name);
      return FAILURE;
   }
   if (!(meta->capabilities & TOOL_CAP_SCHEDULABLE)) {
      if (err_buf && err_buf_size)
         snprintf(err_buf, err_buf_size, "tool '%s' is not schedulable", tool_name);
      return FAILURE;
   }
   if (!tool_registry_is_enabled(tool_name)) {
      if (err_buf && err_buf_size)
         snprintf(err_buf, err_buf_size, "tool '%s' is disabled", tool_name);
      return FAILURE;
   }
   /* Per-action gate: a tool may be schedulable for some actions but not others
    * (e.g. messaging read_* vs send).  Enforced here so create time and fire
    * time share one verdict. */
   if (meta->validate_schedulable_action &&
       meta->validate_schedulable_action(tool_action, err_buf, err_buf_size) != SUCCESS) {
      return FAILURE;
   }
   if ((meta->capabilities & TOOL_CAP_REQUIRES_VALUE) && (!tool_value || !tool_value[0])) {
      if (err_buf && err_buf_size)
         snprintf(err_buf, err_buf_size,
                  "tool '%s' requires a non-empty tool_value (e.g. search query)", tool_name);
      return FAILURE;
   }
   return SUCCESS;
}

const char *tool_registry_resolve_device(const tool_metadata_t *metadata, const char *key) {
   if (!metadata || !key || !metadata->device_map) {
      return NULL;
   }

   for (int i = 0; i < metadata->device_map_count; i++) {
      if (metadata->device_map[i].key && strcasecmp(metadata->device_map[i].key, key) == 0) {
         return metadata->device_map[i].device;
      }
   }
   return NULL;
}

/* =============================================================================
 * Config Integration
 * ============================================================================= */

int tool_registry_parse_configs(const char *config_path) {
   if (!config_path || !s_initialized) {
      return 1;
   }

   /* Open and parse the config file */
   FILE *fp = fopen(config_path, "r");
   if (!fp) {
      OLOG_WARNING("tool_registry: Cannot open config file: %s", config_path);
      return 1;
   }

   char errbuf[256];
   toml_table_t *root = toml_parse_file(fp, errbuf, sizeof(errbuf));
   fclose(fp);

   if (!root) {
      OLOG_ERROR("tool_registry: Failed to parse config: %s", errbuf);
      return 1;
   }

   pthread_mutex_lock(&s_registry_mutex);

   for (int i = 0; i < s_tool_count; i++) {
      tool_entry_t *entry = &s_tools[i];
      if (!entry->registered || !entry->metadata.config_parser || !entry->metadata.config_section) {
         continue;
      }

      /* Look up the tool's TOML section */
      toml_table_t *section = toml_table_in(root, entry->metadata.config_section);

      /* Call the tool's config parser (section may be NULL if not present) */
      entry->metadata.config_parser(section, entry->metadata.config);

      /* For dangerous tools, check if enabled field was explicitly set.
       * Only override the default if the tool has a config section in the TOML —
       * missing section means the user hasn't configured it, so respect the
       * default_local/default_remote flags from tool metadata instead. */
      if ((entry->metadata.capabilities & TOOL_CAP_DANGEROUS) && section) {
         /* Convention: config struct's first field is 'bool enabled' for dangerous tools */
         bool *enabled_ptr = (bool *)entry->metadata.config;
         entry->enabled = *enabled_ptr;
         OLOG_INFO("Dangerous tool '%s' enabled=%s (from config)", entry->metadata.name,
                   entry->enabled ? "true" : "false");
      }
   }

   atomic_fetch_add(&s_generation, 1); /* after the change */
   pthread_mutex_unlock(&s_registry_mutex);

   toml_free(root);
   return SUCCESS;
}

void tool_registry_write_configs(void *fp) {
   if (!fp || !s_initialized) {
      return;
   }

   pthread_mutex_lock(&s_registry_mutex);

   for (int i = 0; i < s_tool_count; i++) {
      tool_entry_t *entry = &s_tools[i];
      if (!entry->registered || !entry->metadata.config_writer || !entry->metadata.config_section ||
          !entry->metadata.config) {
         continue;
      }

      /* Write section header */
      fprintf((FILE *)fp, "\n[%s]\n", entry->metadata.config_section);
      /* Let the tool write its key-value pairs */
      entry->metadata.config_writer(fp, entry->metadata.config);
   }

   pthread_mutex_unlock(&s_registry_mutex);
}

const char *tool_registry_get_secret(const char *tool_name, const char *secret_name) {
   if (!tool_name || !secret_name || !s_initialized) {
      return NULL;
   }

   /* Find the tool */
   const tool_metadata_t *meta = tool_registry_lookup(tool_name);
   if (!meta) {
      OLOG_WARNING("tool_registry: Unknown tool '%s' requesting secret", tool_name);
      return NULL;
   }

   /* Verify tool declared this secret */
   if (!tool_declared_secret(meta, secret_name)) {
      OLOG_WARNING("tool_registry: Tool '%s' did not declare secret '%s'", tool_name, secret_name);
      return NULL;
   }

   /* Get the secret from global secrets */
   const secrets_config_t *secrets = config_get_secrets();
   if (!secrets) {
      return NULL;
   }

   /* Map secret names to struct fields */
   if (strcmp(secret_name, "openai_api_key") == 0) {
      return secrets->openai_api_key[0] ? secrets->openai_api_key : NULL;
   } else if (strcmp(secret_name, "claude_api_key") == 0) {
      return secrets->claude_api_key[0] ? secrets->claude_api_key : NULL;
   } else if (strcmp(secret_name, "gemini_api_key") == 0) {
      return secrets->gemini_api_key[0] ? secrets->gemini_api_key : NULL;
   } else if (strcmp(secret_name, "mqtt_username") == 0) {
      return secrets->mqtt_username[0] ? secrets->mqtt_username : NULL;
   } else if (strcmp(secret_name, "mqtt_password") == 0) {
      return secrets->mqtt_password[0] ? secrets->mqtt_password : NULL;
   } else if (strcmp(secret_name, "home_assistant_token") == 0) {
      return secrets->home_assistant_token[0] ? secrets->home_assistant_token : NULL;
   }

   OLOG_WARNING("tool_registry: Unknown secret name '%s'", secret_name);
   return NULL;
}

const char *tool_registry_get_config_string(const char *path) {
   if (!path) {
      return NULL;
   }

   const dawn_config_t *config = config_get();
   if (!config) {
      return NULL;
   }

   /* Parse path and lookup value */
   /* Format: "section.key" */
   if (strcmp(path, "localization.location") == 0) {
      return config->localization.location[0] ? config->localization.location : NULL;
   } else if (strcmp(path, "localization.timezone") == 0) {
      return config->localization.timezone[0] ? config->localization.timezone : NULL;
   } else if (strcmp(path, "search.endpoint") == 0) {
      return config->search.endpoint[0] ? config->search.endpoint : NULL;
   } else if (strcmp(path, "paths.music_dir") == 0) {
      return config->paths.music_dir[0] ? config->paths.music_dir : NULL;
   } else if (strcmp(path, "general.ai_name") == 0) {
      return config->general.ai_name[0] ? config->general.ai_name : NULL;
   }

   OLOG_WARNING("tool_registry: Unknown config path '%s'", path);
   return NULL;
}

/* =============================================================================
 * Iteration Functions
 * ============================================================================= */

void tool_registry_foreach(tool_foreach_callback_t callback, void *user_data) {
   if (!callback || !s_initialized) {
      return;
   }

   pthread_mutex_lock(&s_registry_mutex);

   for (int i = 0; i < s_tool_count; i++) {
      if (s_tools[i].registered) {
         callback(&s_tools[i].metadata, user_data);
      }
   }

   pthread_mutex_unlock(&s_registry_mutex);
}

int tool_registry_count(void) {
   return s_tool_count;
}

const tool_metadata_t *tool_registry_get_by_index(int index) {
   if (index < 0 || index >= s_tool_count) {
      return NULL;
   }
   return &s_tools[index].metadata;
}

int tool_registry_enabled_count(void) {
   int count = 0;
   pthread_mutex_lock(&s_registry_mutex);

   for (int i = 0; i < s_tool_count; i++) {
      if (s_tools[i].registered && s_tools[i].enabled) {
         count++;
      }
   }

   pthread_mutex_unlock(&s_registry_mutex);
   return count;
}

/* =============================================================================
 * Capability Queries
 * ============================================================================= */

bool tool_registry_has_capability(const char *name, tool_capability_t cap) {
   const tool_metadata_t *meta = tool_registry_find(name);
   if (!meta) {
      return false;
   }
   return (meta->capabilities & cap) != 0;
}

void tool_registry_foreach_with_capability(tool_capability_t cap,
                                           tool_foreach_callback_t callback,
                                           void *user_data) {
   if (!callback || !s_initialized) {
      return;
   }

   pthread_mutex_lock(&s_registry_mutex);

   for (int i = 0; i < s_tool_count; i++) {
      if (s_tools[i].registered && (s_tools[i].metadata.capabilities & cap)) {
         callback(&s_tools[i].metadata, user_data);
      }
   }

   pthread_mutex_unlock(&s_registry_mutex);
}

/* =============================================================================
 * Dynamic Enum Override Functions
 * ============================================================================= */

/**
 * @brief Find existing override for a tool/param combination
 * @return Index into s_enum_overrides, or -1 if not found
 * @note Must be called with registry mutex held
 */
static int find_enum_override(int tool_index, int param_index) {
   for (int i = 0; i < MAX_ENUM_OVERRIDES; i++) {
      if (s_enum_overrides[i].active && s_enum_overrides[i].tool_index == tool_index &&
          s_enum_overrides[i].param_index == param_index) {
         return i;
      }
   }
   return -1;
}

/**
 * @brief Allocate a new override slot
 * @return Index into s_enum_overrides, or -1 if full
 * @note Must be called with registry mutex held
 */
static int alloc_enum_override(void) {
   for (int i = 0; i < MAX_ENUM_OVERRIDES; i++) {
      if (!s_enum_overrides[i].active) {
         s_override_count++;
         return i;
      }
   }
   return -1;
}

/**
 * @brief Sanitize an enum value string (security)
 *
 * Only allows alphanumeric characters, underscores, and hyphens.
 * Prevents injection of control characters or special characters
 * that could affect LLM prompt generation.
 *
 * @param dest Destination buffer
 * @param src Source string
 * @param max_len Maximum length including null terminator
 * @return true if sanitization succeeded, false if value was rejected
 */
static bool sanitize_enum_value(char *dest, const char *src, size_t max_len) {
   if (!dest || !src || max_len == 0) {
      return false;
   }

   /* Check if source is empty */
   if (src[0] == '\0') {
      return false;
   }

   size_t j = 0;
   for (size_t i = 0; src[i] != '\0' && j < max_len - 1; i++) {
      char c = src[i];
      /* Allow alphanumeric, underscore, hyphen, space */
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
          c == '-' || c == ' ') {
         dest[j++] = c;
      }
      /* Skip invalid characters silently */
   }
   dest[j] = '\0';

   /* Reject if result is empty after sanitization */
   if (j == 0) {
      return false;
   }

   return true;
}

/**
 * @brief Get the effective parameter for a tool
 *
 * Checks if there's an override for this parameter and returns it,
 * otherwise returns the original parameter.
 *
 * @note Must be called with registry mutex held
 */
static const treg_param_t *get_effective_param(int tool_index, int param_index) {
   int override_idx = find_enum_override(tool_index, param_index);
   if (override_idx >= 0) {
      return &s_enum_overrides[override_idx].override_param;
   }
   return &s_tools[tool_index].metadata.params[param_index];
}

int tool_registry_update_param_enum(const char *tool_name,
                                    const char *param_name,
                                    const char **values,
                                    int count) {
   if (!tool_name || !param_name || !values || count <= 0) {
      return TREG_ENUM_RC_FAILURE;
   }

   if (count > TOOL_PARAM_ENUM_MAX) {
      OLOG_ERROR("tool_registry: Enum count %d exceeds max %d", count, TOOL_PARAM_ENUM_MAX);
      return TREG_ENUM_RC_TOO_MANY;
   }

   pthread_mutex_lock(&s_registry_mutex);

   if (!s_initialized) {
      pthread_mutex_unlock(&s_registry_mutex);
      return TREG_ENUM_RC_FAILURE;
   }

   /* Find the tool by name */
   int tool_idx = hash_lookup(s_name_hash, HASH_BUCKETS, tool_name);
   if (tool_idx < 0 || !s_tools[tool_idx].registered) {
      OLOG_ERROR("tool_registry: Tool '%s' not found for enum update", tool_name);
      pthread_mutex_unlock(&s_registry_mutex);
      return TREG_ENUM_RC_FAILURE;
   }

   tool_entry_t *entry = &s_tools[tool_idx];
   const tool_metadata_t *meta = &entry->metadata;

   /* Find the parameter by name */
   int param_idx = -1;
   for (int i = 0; i < meta->param_count; i++) {
      if (meta->params[i].name && strcmp(meta->params[i].name, param_name) == 0) {
         param_idx = i;
         break;
      }
   }

   if (param_idx < 0) {
      OLOG_ERROR("tool_registry: Parameter '%s' not found in tool '%s'", param_name, tool_name);
      pthread_mutex_unlock(&s_registry_mutex);
      return TREG_ENUM_RC_PARAM_NOT_FOUND;
   }

   const treg_param_t *orig_param = &meta->params[param_idx];
   if (orig_param->type != TOOL_PARAM_TYPE_ENUM) {
      OLOG_ERROR("tool_registry: Parameter '%s' in tool '%s' is not enum type", param_name,
                 tool_name);
      pthread_mutex_unlock(&s_registry_mutex);
      return TREG_ENUM_RC_NOT_ENUM;
   }

   /* Find or allocate override slot */
   int override_idx = find_enum_override(tool_idx, param_idx);
   if (override_idx < 0) {
      override_idx = alloc_enum_override();
      if (override_idx < 0) {
         OLOG_ERROR("tool_registry: Enum override slots exhausted");
         pthread_mutex_unlock(&s_registry_mutex);
         return TREG_ENUM_RC_SLOTS_EXHAUSTED;
      }
   }

   enum_override_t *override = &s_enum_overrides[override_idx];

   /* Clear and setup the override */
   memset(override->values, 0, sizeof(override->values));
   override->tool_index = tool_idx;
   override->param_index = param_idx;
   override->count = count;
   override->active = true;

   /* Deep copy and sanitize enum values */
   int valid_count = 0;
   for (int i = 0; i < count; i++) {
      if (values[i]) {
         if (sanitize_enum_value(override->values[valid_count], values[i], TOOL_NAME_MAX)) {
            override->value_ptrs[valid_count] = override->values[valid_count];
            valid_count++;
         } else {
            OLOG_WARNING("tool_registry: Rejected invalid enum value for %s.%s", tool_name,
                         param_name);
         }
      }
   }
   /* Null out remaining pointers */
   for (int i = valid_count; i < TOOL_PARAM_ENUM_MAX; i++) {
      override->value_ptrs[i] = NULL;
   }

   /* Copy original param and update with override pointers */
   override->override_param = *orig_param;
   for (int i = 0; i < TOOL_PARAM_ENUM_MAX; i++) {
      override->override_param.enum_values[i] = override->value_ptrs[i];
   }
   override->override_param.enum_count = valid_count;

   /* Invalidate schema cache */
   s_cache_valid = false;
   atomic_fetch_add(&s_generation, 1);

   OLOG_INFO("tool_registry: Updated enum for %s.%s with %d values (%d sanitized)", tool_name,
             param_name, valid_count, count - valid_count);

   pthread_mutex_unlock(&s_registry_mutex);
   return TREG_ENUM_RC_OK;
}

void tool_registry_invalidate_cache(void) {
   pthread_mutex_lock(&s_registry_mutex);
   s_cache_valid = false;
   atomic_fetch_add(&s_generation, 1);
   pthread_mutex_unlock(&s_registry_mutex);

   /* Also invalidate LLM tools cache and system-prompt hint for coherence */
   llm_tools_invalidate_cache();
   invalidate_system_instructions();

   OLOG_INFO("tool_registry: Schema cache invalidated (including LLM tools and prompt)");
}

uint64_t tool_registry_generation(void) {
   return atomic_load(&s_generation);
}

bool tool_registry_is_cache_valid(void) {
   pthread_mutex_lock(&s_registry_mutex);
   bool valid = s_cache_valid;
   pthread_mutex_unlock(&s_registry_mutex);
   return valid;
}

const treg_param_t *tool_registry_get_effective_param(const char *tool_name, int param_index) {
   if (!tool_name || param_index < 0) {
      return NULL;
   }

   pthread_mutex_lock(&s_registry_mutex);

   if (!s_initialized) {
      pthread_mutex_unlock(&s_registry_mutex);
      return NULL;
   }

   /* Find the tool by name */
   int tool_idx = hash_lookup(s_name_hash, HASH_BUCKETS, tool_name);
   if (tool_idx < 0 || !s_tools[tool_idx].registered) {
      pthread_mutex_unlock(&s_registry_mutex);
      return NULL;
   }

   const tool_metadata_t *meta = &s_tools[tool_idx].metadata;
   if (param_index >= meta->param_count) {
      pthread_mutex_unlock(&s_registry_mutex);
      return NULL;
   }

   const treg_param_t *result = get_effective_param(tool_idx, param_index);
   pthread_mutex_unlock(&s_registry_mutex);
   return result;
}

const char *tool_registry_get_action_param_name(const char *tool_name) {
   if (!tool_name) {
      return NULL;
   }

   const tool_metadata_t *meta = tool_registry_find(tool_name);
   if (!meta) {
      return NULL;
   }

   for (int i = 0; i < meta->param_count; i++) {
      if (meta->params[i].maps_to == TOOL_MAPS_TO_ACTION) {
         return meta->params[i].name;
      }
   }
   return NULL;
}

bool tool_registry_action_is_repeatable(const char *tool_name, const char *action) {
   if (!tool_name || !action || action[0] == '\0') {
      return false;
   }

   /* tool_registry_find handles name-or-alias.  Registration completes at
    * startup before any concurrent LLM traffic, and metadata content is never
    * mutated afterward, so reading repeatable_actions[] without the registry
    * lock is safe (same contract as the other lock-free metadata readers). */
   const tool_metadata_t *meta = tool_registry_find(tool_name);
   if (!meta) {
      return false;
   }

   int count = meta->repeatable_action_count;
   if (count > TOOL_REPEATABLE_ACTIONS_MAX) {
      count = TOOL_REPEATABLE_ACTIONS_MAX; /* defensive: clamp to array size */
   }
   for (int i = 0; i < count; i++) {
      if (meta->repeatable_actions[i] && strcmp(meta->repeatable_actions[i], action) == 0) {
         return true;
      }
   }
   return false;
}

/* =============================================================================
 * Direct Command Variation Statistics
 * ============================================================================= */

/**
 * @brief Count pattern variations for a single device type
 *
 * Sums all pattern_count values across all actions for the type.
 */
static int count_device_type_patterns(tool_device_type_t device_type) {
   const device_type_def_t *type_def = device_type_get_def(device_type);
   if (!type_def) {
      return 0;
   }

   int total = 0;
   for (int a = 0; a < type_def->action_count; a++) {
      total += type_def->actions[a].pattern_count;
   }
   return total;
}

int tool_registry_count_tool_variations(const char *name) {
   if (!name) {
      return 0;
   }

   pthread_mutex_lock(&s_registry_mutex);

   if (!s_initialized) {
      pthread_mutex_unlock(&s_registry_mutex);
      return 0;
   }

   /* Find the tool by name */
   int tool_idx = hash_lookup(s_name_hash, HASH_BUCKETS, name);
   if (tool_idx < 0 || !s_tools[tool_idx].registered) {
      pthread_mutex_unlock(&s_registry_mutex);
      return 0;
   }

   const tool_metadata_t *meta = &s_tools[tool_idx].metadata;

   /* Get pattern count for this device type */
   int patterns = count_device_type_patterns(meta->device_type);

   /* Multiply by name variations: 1 (primary name) + alias_count */
   int name_variations = 1 + meta->alias_count;

   pthread_mutex_unlock(&s_registry_mutex);

   return patterns * name_variations;
}

int tool_registry_count_variations(void) {
   pthread_mutex_lock(&s_registry_mutex);

   if (!s_initialized) {
      pthread_mutex_unlock(&s_registry_mutex);
      return 0;
   }

   int total = 0;
   for (int i = 0; i < s_tool_count; i++) {
      if (!s_tools[i].registered) {
         continue;
      }

      const tool_metadata_t *meta = &s_tools[i].metadata;

      /* Get pattern count for this device type */
      int patterns = count_device_type_patterns(meta->device_type);

      /* Multiply by name variations: 1 (primary name) + alias_count */
      int name_variations = 1 + meta->alias_count;

      total += patterns * name_variations;
   }

   pthread_mutex_unlock(&s_registry_mutex);
   return total;
}

/* =============================================================================
 * Kinds of Action
 * ============================================================================= */

const char *tool_action_kind_name(tool_action_kind_t kind) {
   switch (kind) {
      case TOOL_KIND_READ:
         return "read";
      case TOOL_KIND_FETCH:
         return "fetch";
      case TOOL_KIND_STATE:
         return "state";
      case TOOL_KIND_DEVICE:
         return "device";
      case TOOL_KIND_PREPARE:
         return "prepare";
      case TOOL_KIND_ACT:
         break;
   }
   return "act";
}

static const tool_action_kind_entry_t *find_action_kind(const tool_metadata_t *meta,
                                                        const char *action) {
   for (int i = 0; i < meta->action_kind_count; i++) {
      if (strcmp(meta->action_kinds[i].action, action) == 0) {
         return &meta->action_kinds[i];
      }
   }
   return NULL;
}

const char *tool_third_party_frame(const tool_metadata_t *meta, const char *action) {
   if (!meta) {
      return NULL;
   }
   /* An entry with no frame of its own keeps the tool's: listing an action
    * (for its kind) never drops the frame its text goes in. */
   const tool_action_kind_entry_t *e = action && action[0] ? find_action_kind(meta, action) : NULL;
   return e && e->third_party ? e->third_party : meta->third_party;
}

const char *tool_third_party_frame_any(const tool_metadata_t *meta) {
   if (!meta) {
      return NULL;
   }
   if (meta->third_party) {
      return meta->third_party;
   }
   for (int i = 0; i < meta->action_kind_count; i++) {
      if (meta->action_kinds[i].third_party) {
         return meta->action_kinds[i].third_party;
      }
   }
   return NULL;
}

const char *tool_default_action(const tool_metadata_t *meta) {
   if (!meta) {
      return "get";
   }
   switch (meta->device_type) {
      case TOOL_DEVICE_TYPE_BOOLEAN:
         return "toggle";
      case TOOL_DEVICE_TYPE_ANALOG:
         return "set";
      case TOOL_DEVICE_TYPE_GETTER:
         return "get";
      case TOOL_DEVICE_TYPE_TRIGGER:
         return "trigger";
      case TOOL_DEVICE_TYPE_MUSIC:
         return "play";
      default:
         return "get";
   }
}

const char *tool_effective_action(const tool_metadata_t *meta, const char *action) {
   if (action && action[0] != '\0') {
      return action;
   }
   if (!meta) {
      return "get";
   }
   if (meta->sync_wait && strcmp(meta->name, "viewing") == 0) {
      return ""; /* the viewing sync path sends none */
   }
   if (meta->mqtt_only) {
      return tool_default_action(meta);
   }
   /* No callback: command_execute picks the default of the tool it resolves
    * the device to. */
   return meta->callback ? "get" : "";
}

/* Whether @p action is the confirm some PREPARE entry names. */
static bool is_a_confirm(const tool_metadata_t *meta, const char *action) {
   for (int i = 0; i < meta->action_kind_count; i++) {
      const char *c = meta->action_kinds[i].confirm;
      if (c && strcmp(c, action) == 0) {
         return true;
      }
   }
   return false;
}

tool_action_kind_t tool_action_kind(const tool_metadata_t *meta,
                                    const char *device,
                                    const char *action,
                                    const char *value) {
   if (!meta) {
      return TOOL_KIND_ACT;
   }
   tool_action_kind_t kind = meta->default_kind;
   if (action && action[0]) {
      const tool_action_kind_entry_t *e = find_action_kind(meta, action);
      if (e) {
         kind = e->kind;
      }
   }
   if (meta->classify_call) {
      kind = meta->classify_call(device, action, value, kind);
      if ((unsigned)kind > (unsigned)TOOL_KIND_PREPARE) {
         kind = TOOL_KIND_ACT; /* not a kind: deny by default */
      }
   }
   /* A confirm acts, whatever a classifier says: it carries out what its
    * prepare staged. */
   if (action && action[0] && kind != TOOL_KIND_ACT && is_a_confirm(meta, action)) {
      kind = TOOL_KIND_ACT;
   }
   return kind;
}

/* The tool's ENUM action parameter, or NULL. */
static const treg_param_t *enum_action_param(const tool_metadata_t *meta) {
   for (int i = 0; i < meta->param_count; i++) {
      const treg_param_t *p = &meta->params[i];
      if (p->maps_to == TOOL_MAPS_TO_ACTION && p->type == TOOL_PARAM_TYPE_ENUM) {
         return p;
      }
   }
   return NULL;
}

static bool param_has_value(const treg_param_t *p, const char *value) {
   for (int i = 0; i < p->enum_count && i < TOOL_PARAM_ENUM_MAX; i++) {
      if (p->enum_values[i] && strcmp(p->enum_values[i], value) == 0) {
         return true;
      }
   }
   return false;
}

bool tool_action_canonical(const tool_metadata_t *meta,
                           const char *action,
                           char *out,
                           size_t out_len) {
   if (!meta || !action || !out || out_len == 0) {
      return false;
   }
   const treg_param_t *param = enum_action_param(meta);
   if (!param) {
      snprintf(out, out_len, "%s", action);
      return true;
   }
   for (int i = 0; i < param->enum_count && i < TOOL_PARAM_ENUM_MAX; i++) {
      if (param->enum_values[i] && strcasecmp(param->enum_values[i], action) == 0) {
         snprintf(out, out_len, "%s", param->enum_values[i]);
         return true;
      }
   }
   out[0] = '\0';
   return false;
}

void tool_action_list(const tool_metadata_t *meta, char *out, size_t out_len) {
   if (!out || out_len == 0) {
      return;
   }
   out[0] = '\0';
   const treg_param_t *param = meta ? enum_action_param(meta) : NULL;
   size_t used = 0;
   for (int i = 0; param && i < param->enum_count && i < TOOL_PARAM_ENUM_MAX; i++) {
      if (!param->enum_values[i]) {
         continue;
      }
      const int n = snprintf(out + used, out_len - used, "%s%s", used ? ", " : "",
                             param->enum_values[i]);
      if (n < 0 || (size_t)n >= out_len - used) {
         break;
      }
      used += (size_t)n;
   }
}

int tool_action_kinds_validate(const tool_metadata_t *meta, char *why, size_t why_len) {
   char scratch[1];
   if (!why || why_len == 0) {
      why = scratch;
      why_len = sizeof(scratch);
   }
   why[0] = '\0';
   if (!meta) {
      snprintf(why, why_len, "no tool");
      return FAILURE;
   }
   if ((unsigned)meta->default_kind >= (unsigned)TOOL_KIND_PREPARE) {
      snprintf(why, why_len, "default_kind must be a kind other than prepare");
      return FAILURE;
   }
   if (meta->action_kind_count == 0) {
      return SUCCESS;
   }
   if (meta->action_kind_count < 0 || !meta->action_kinds) {
      snprintf(why, why_len, "action_kinds table missing");
      return FAILURE;
   }
   const treg_param_t *param = enum_action_param(meta);
   if (!param) {
      snprintf(why, why_len, "lists actions but has no ENUM action parameter");
      return FAILURE;
   }
   for (int i = 0; i < meta->action_kind_count; i++) {
      const tool_action_kind_entry_t *e = &meta->action_kinds[i];
      if (!e->action || !param_has_value(param, e->action)) {
         snprintf(why, why_len, "'%s' is not one of its actions", e->action ? e->action : "");
         return FAILURE;
      }
      if ((unsigned)e->kind > (unsigned)TOOL_KIND_PREPARE) {
         snprintf(why, why_len, "'%s' has no valid kind", e->action);
         return FAILURE;
      }
      for (int j = 0; j < i; j++) {
         if (strcmp(meta->action_kinds[j].action, e->action) == 0) {
            snprintf(why, why_len, "'%s' is listed twice", e->action);
            return FAILURE;
         }
      }
      if (e->kind != TOOL_KIND_PREPARE) {
         if (e->confirm) {
            snprintf(why, why_len, "'%s' names a confirm but isn't prepare", e->action);
            return FAILURE;
         }
         continue;
      }
      const tool_action_kind_entry_t *c = e->confirm ? find_action_kind(meta, e->confirm) : NULL;
      if (!c || c->kind != TOOL_KIND_ACT) {
         snprintf(why, why_len, "prepare '%s' needs a listed confirm that acts", e->action);
         return FAILURE;
      }
   }
   return SUCCESS;
}
