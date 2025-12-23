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
 * Unified Command Registry Implementation
 *
 * Parses commands_config_nuevo.json and builds a registry of all available
 * commands with their metadata for unified execution.
 */

#include "core/command_registry.h"

#include <json-c/json.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "logging.h"
#include "mosquitto_comms.h"
#include "tools/string_utils.h"

/* =============================================================================
 * Module State
 * ============================================================================= */

static cmd_definition_t s_commands[CMD_MAX_DEVICES];
static int s_command_count = 0;
static int s_enabled_count = 0; /* Cached enabled count, updated during init */
static bool s_initialized = false;

/* Alias lookup table for fast device resolution */
#define MAX_ALIASES_TOTAL 256
typedef struct {
   char alias[CMD_NAME_LEN];
   int command_index; /* Index into s_commands */
} alias_entry_t;

static alias_entry_t s_aliases[MAX_ALIASES_TOTAL];
static int s_alias_count = 0;

/* =============================================================================
 * Hash Table for O(1) Lookups
 * ============================================================================= */

#define HASH_BUCKETS 128 /* Power of 2 for fast modulo */

typedef struct {
   char key[CMD_NAME_LEN];
   int value; /* Index into s_commands, or -1 if empty */
} hash_entry_t;

static hash_entry_t s_name_hash[HASH_BUCKETS];
static hash_entry_t s_alias_hash[HASH_BUCKETS * 2]; /* More aliases than commands */

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
         safe_strncpy(table[probe].key, key, CMD_NAME_LEN);
         table[probe].value = value;
         return;
      }
   }
   LOG_WARNING("command_registry: Hash table full, could not insert '%s'", key);
}

/**
 * @brief Lookup in hash table with open addressing
 * @return Index into s_commands, or -1 if not found
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
   }
   for (size_t i = 0; i < HASH_BUCKETS * 2; i++) {
      s_alias_hash[i].value = -1;
   }
}

/* =============================================================================
 * Helper Functions
 * ============================================================================= */

/**
 * @brief Register a command in the registry
 */
static cmd_definition_t *register_command(const char *name, const char *topic) {
   if (s_command_count >= CMD_MAX_DEVICES) {
      LOG_ERROR("command_registry: Maximum command count (%d) reached", CMD_MAX_DEVICES);
      return NULL;
   }

   int cmd_index = s_command_count;
   cmd_definition_t *cmd = &s_commands[cmd_index];
   memset(cmd, 0, sizeof(*cmd));

   safe_strncpy(cmd->name, name, CMD_NAME_LEN);
   safe_strncpy(cmd->device_string, name, CMD_NAME_LEN); /* Default: same as name */
   safe_strncpy(cmd->topic, topic, CMD_TOPIC_LEN);
   cmd->enabled = true;

   /* Check if this device has a callback */
   device_callback_fn callback = get_device_callback(name);
   cmd->has_callback = (callback != NULL);
   cmd->mqtt_only = !cmd->has_callback;

   /* Insert into name hash table for O(1) lookup */
   hash_insert(s_name_hash, HASH_BUCKETS, name, cmd_index);

   s_command_count++;
   return cmd;
}

/**
 * @brief Register an alias for a command
 */
static void register_alias(const char *alias, int command_index) {
   if (s_alias_count >= MAX_ALIASES_TOTAL) {
      LOG_WARNING("command_registry: Maximum alias count reached, skipping '%s'", alias);
      return;
   }

   safe_strncpy(s_aliases[s_alias_count].alias, alias, CMD_NAME_LEN);
   s_aliases[s_alias_count].command_index = command_index;
   s_alias_count++;

   /* Insert into alias hash table for O(1) lookup */
   hash_insert(s_alias_hash, HASH_BUCKETS * 2, alias, command_index);
}

/**
 * @brief Find command index by name (O(1) via hash table)
 */
static int find_command_index(const char *name) {
   return hash_lookup(s_name_hash, HASH_BUCKETS, name);
}

/**
 * @brief Find command by alias (O(1) via hash table)
 */
static int find_by_alias(const char *alias) {
   return hash_lookup(s_alias_hash, HASH_BUCKETS * 2, alias);
}

/**
 * @brief Parse devices from JSON
 */
static int parse_devices(struct json_object *devices_obj) {
   struct json_object_iterator it = json_object_iter_begin(devices_obj);
   struct json_object_iterator it_end = json_object_iter_end(devices_obj);

   while (!json_object_iter_equal(&it, &it_end)) {
      const char *device_name = json_object_iter_peek_name(&it);
      struct json_object *device_obj = json_object_iter_peek_value(&it);

      /* Get topic (required) */
      struct json_object *topic_obj = NULL;
      const char *topic = "dawn"; /* Default topic */
      if (json_object_object_get_ex(device_obj, "topic", &topic_obj)) {
         topic = json_object_get_string(topic_obj);
      }

      /* Register the command */
      cmd_definition_t *cmd = register_command(device_name, topic);
      if (!cmd) {
         json_object_iter_next(&it);
         continue;
      }

      int cmd_index = s_command_count - 1;

      /* Register the device name itself as an alias for lookup */
      register_alias(device_name, cmd_index);

      /* Parse aliases */
      struct json_object *aliases_obj = NULL;
      if (json_object_object_get_ex(device_obj, "aliases", &aliases_obj)) {
         int alias_count = json_object_array_length(aliases_obj);
         for (int i = 0; i < alias_count; i++) {
            struct json_object *alias_obj = json_object_array_get_idx(aliases_obj, i);
            const char *alias = json_object_get_string(alias_obj);
            if (alias) {
               register_alias(alias, cmd_index);
            }
         }
      }

      /* Check for special flags */
      struct json_object *flag_obj = NULL;
      if (json_object_object_get_ex(device_obj, "sync_wait", &flag_obj)) {
         cmd->sync_wait = json_object_get_boolean(flag_obj);
      }
      if (json_object_object_get_ex(device_obj, "skip_followup", &flag_obj)) {
         cmd->skip_followup = json_object_get_boolean(flag_obj);
      }
      if (json_object_object_get_ex(device_obj, "mqtt_only", &flag_obj)) {
         cmd->mqtt_only = json_object_get_boolean(flag_obj);
      }

      /* Check device type (getter vs setter) */
      struct json_object *type_obj = NULL;
      if (json_object_object_get_ex(device_obj, "type", &type_obj)) {
         const char *type_str = json_object_get_string(type_obj);
         cmd->is_getter = (type_str && strcmp(type_str, "getter") == 0);
      }

      /* Parse tool definition if present */
      struct json_object *tool_obj = NULL;
      if (json_object_object_get_ex(device_obj, "tool", &tool_obj)) {
         /* Get tool name (overrides device name for API calls) */
         struct json_object *name_obj = NULL;
         if (json_object_object_get_ex(tool_obj, "name", &name_obj)) {
            safe_strncpy(cmd->name, json_object_get_string(name_obj), CMD_NAME_LEN);
            /* Register explicit tool name for lookup */
            hash_insert(s_name_hash, HASH_BUCKETS, cmd->name, cmd_index);
         } else {
            /* No explicit name - sanitize device name for OpenAI compatibility
             * OpenAI pattern: ^[a-zA-Z0-9_-]+
             * Replace spaces with underscores */
            char *p = cmd->name;
            while (*p) {
               if (*p == ' ') {
                  *p = '_';
               }
               p++;
            }
            /* Register sanitized name for lookup (original already registered) */
            hash_insert(s_name_hash, HASH_BUCKETS, cmd->name, cmd_index);
         }

         /* Get description */
         struct json_object *desc_obj = NULL;
         if (json_object_object_get_ex(tool_obj, "description", &desc_obj)) {
            safe_strncpy(cmd->description, json_object_get_string(desc_obj), CMD_DESC_LEN);
         }

         /* Parse parameters */
         struct json_object *params_obj = NULL;
         if (json_object_object_get_ex(tool_obj, "parameters", &params_obj)) {
            int param_count = json_object_array_length(params_obj);
            if (param_count > CMD_MAX_PARAMS) {
               param_count = CMD_MAX_PARAMS;
            }

            for (int i = 0; i < param_count; i++) {
               struct json_object *param_obj = json_object_array_get_idx(params_obj, i);
               cmd_param_t *param = &cmd->parameters[cmd->param_count];

               struct json_object *val = NULL;
               if (json_object_object_get_ex(param_obj, "name", &val)) {
                  safe_strncpy(param->name, json_object_get_string(val), CMD_NAME_LEN);
               }
               if (json_object_object_get_ex(param_obj, "description", &val)) {
                  safe_strncpy(param->description, json_object_get_string(val),
                               sizeof(param->description));
               }
               if (json_object_object_get_ex(param_obj, "required", &val)) {
                  param->required = json_object_get_boolean(val);
               }

               /* Parse type */
               if (json_object_object_get_ex(param_obj, "type", &val)) {
                  const char *type_str = json_object_get_string(val);
                  if (strcmp(type_str, "string") == 0) {
                     param->type = CMD_PARAM_STRING;
                  } else if (strcmp(type_str, "integer") == 0) {
                     param->type = CMD_PARAM_INTEGER;
                  } else if (strcmp(type_str, "number") == 0) {
                     param->type = CMD_PARAM_NUMBER;
                  } else if (strcmp(type_str, "boolean") == 0) {
                     param->type = CMD_PARAM_BOOLEAN;
                  } else if (strcmp(type_str, "enum") == 0) {
                     param->type = CMD_PARAM_ENUM;
                  }
               }

               /* Parse enum values */
               if (json_object_object_get_ex(param_obj, "enum", &val)) {
                  int enum_count = json_object_array_length(val);
                  if (enum_count > CMD_MAX_ENUM_VALUES) {
                     enum_count = CMD_MAX_ENUM_VALUES;
                  }
                  for (int j = 0; j < enum_count; j++) {
                     struct json_object *enum_val = json_object_array_get_idx(val, j);
                     safe_strncpy(param->enum_values[j], json_object_get_string(enum_val), 64);
                  }
                  param->enum_count = enum_count;
               }

               /* Parse maps_to */
               if (json_object_object_get_ex(param_obj, "maps_to", &val)) {
                  const char *maps_str = json_object_get_string(val);
                  if (strcmp(maps_str, "value") == 0) {
                     param->maps_to = CMD_MAPS_TO_VALUE;
                  } else if (strcmp(maps_str, "action") == 0) {
                     param->maps_to = CMD_MAPS_TO_ACTION;
                  } else if (strcmp(maps_str, "device") == 0) {
                     param->maps_to = CMD_MAPS_TO_DEVICE;
                  } else {
                     param->maps_to = CMD_MAPS_TO_CUSTOM;
                     safe_strncpy(param->field_name, maps_str, CMD_NAME_LEN);
                  }
               } else {
                  param->maps_to = CMD_MAPS_TO_VALUE; /* Default */
               }

               cmd->param_count++;
            }
         }
      }

      json_object_iter_next(&it);
   }

   return 0;
}

/**
 * @brief Parse meta-tools from JSON "tools" section
 *
 * Meta-tools aggregate multiple devices under a single tool name.
 * They have device_map entries that map parameter values to actual devices.
 */
static int parse_tools(struct json_object *tools_obj) {
   struct json_object_iterator it = json_object_iter_begin(tools_obj);
   struct json_object_iterator it_end = json_object_iter_end(tools_obj);

   while (!json_object_iter_equal(&it, &it_end)) {
      const char *tool_name = json_object_iter_peek_name(&it);
      struct json_object *tool_obj = json_object_iter_peek_value(&it);

      /* Skip comment entries */
      if (tool_name[0] == '_') {
         json_object_iter_next(&it);
         continue;
      }

      /* Get topic (optional, default to "dawn") */
      struct json_object *topic_obj = NULL;
      const char *topic = "dawn";
      if (json_object_object_get_ex(tool_obj, "topic", &topic_obj)) {
         topic = json_object_get_string(topic_obj);
      }

      /* Register the meta-tool as a command */
      cmd_definition_t *cmd = register_command(tool_name, topic);
      if (!cmd) {
         json_object_iter_next(&it);
         continue;
      }

      cmd->is_meta_tool = true;
      cmd->mqtt_only = false; /* Meta-tools dispatch to other commands */

      /* Sanitize tool name for OpenAI compatibility (replace spaces with underscores) */
      char *p = cmd->name;
      bool had_spaces = false;
      while (*p) {
         if (*p == ' ') {
            *p = '_';
            had_spaces = true;
         }
         p++;
      }

      int cmd_index = s_command_count - 1;

      /* Register sanitized name if it differs from original */
      if (had_spaces) {
         hash_insert(s_name_hash, HASH_BUCKETS, cmd->name, cmd_index);
      }

      register_alias(tool_name, cmd_index);

      /* Get description */
      struct json_object *desc_obj = NULL;
      if (json_object_object_get_ex(tool_obj, "description", &desc_obj)) {
         safe_strncpy(cmd->description, json_object_get_string(desc_obj), CMD_DESC_LEN);
      }

      /* Check for skip_followup */
      struct json_object *flag_obj = NULL;
      if (json_object_object_get_ex(tool_obj, "skip_followup", &flag_obj)) {
         cmd->skip_followup = json_object_get_boolean(flag_obj);
      }

      /* Parse device_map */
      struct json_object *device_map_obj = NULL;
      if (json_object_object_get_ex(tool_obj, "device_map", &device_map_obj)) {
         struct json_object_iterator dm_it = json_object_iter_begin(device_map_obj);
         struct json_object_iterator dm_end = json_object_iter_end(device_map_obj);

         while (!json_object_iter_equal(&dm_it, &dm_end) &&
                cmd->device_map_count < CMD_MAX_DEVICE_MAP) {
            const char *key = json_object_iter_peek_name(&dm_it);
            struct json_object *val = json_object_iter_peek_value(&dm_it);
            const char *device = json_object_get_string(val);

            cmd_device_map_t *entry = &cmd->device_map[cmd->device_map_count];
            safe_strncpy(entry->key, key, CMD_NAME_LEN);
            safe_strncpy(entry->device, device, CMD_NAME_LEN);
            cmd->device_map_count++;

            json_object_iter_next(&dm_it);
         }
      }

      /* Parse parameters */
      struct json_object *params_obj = NULL;
      if (json_object_object_get_ex(tool_obj, "parameters", &params_obj)) {
         int param_count = json_object_array_length(params_obj);
         if (param_count > CMD_MAX_PARAMS) {
            param_count = CMD_MAX_PARAMS;
         }

         for (int i = 0; i < param_count; i++) {
            struct json_object *param_obj = json_object_array_get_idx(params_obj, i);
            cmd_param_t *param = &cmd->parameters[cmd->param_count];

            struct json_object *val = NULL;
            if (json_object_object_get_ex(param_obj, "name", &val)) {
               safe_strncpy(param->name, json_object_get_string(val), CMD_NAME_LEN);
            }
            if (json_object_object_get_ex(param_obj, "description", &val)) {
               safe_strncpy(param->description, json_object_get_string(val),
                            sizeof(param->description));
            }
            if (json_object_object_get_ex(param_obj, "required", &val)) {
               param->required = json_object_get_boolean(val);
            }

            /* Parse type */
            if (json_object_object_get_ex(param_obj, "type", &val)) {
               const char *type_str = json_object_get_string(val);
               if (strcmp(type_str, "string") == 0) {
                  param->type = CMD_PARAM_STRING;
               } else if (strcmp(type_str, "integer") == 0) {
                  param->type = CMD_PARAM_INTEGER;
               } else if (strcmp(type_str, "number") == 0) {
                  param->type = CMD_PARAM_NUMBER;
               } else if (strcmp(type_str, "boolean") == 0) {
                  param->type = CMD_PARAM_BOOLEAN;
               } else if (strcmp(type_str, "enum") == 0) {
                  param->type = CMD_PARAM_ENUM;
               }
            }

            /* Parse enum values */
            if (json_object_object_get_ex(param_obj, "enum", &val)) {
               int enum_count = json_object_array_length(val);
               if (enum_count > CMD_MAX_ENUM_VALUES) {
                  enum_count = CMD_MAX_ENUM_VALUES;
               }
               for (int j = 0; j < enum_count; j++) {
                  struct json_object *enum_val = json_object_array_get_idx(val, j);
                  safe_strncpy(param->enum_values[j], json_object_get_string(enum_val), 64);
               }
               param->enum_count = enum_count;
            }

            /* Parse maps_to */
            if (json_object_object_get_ex(param_obj, "maps_to", &val)) {
               const char *maps_str = json_object_get_string(val);
               if (strcmp(maps_str, "value") == 0) {
                  param->maps_to = CMD_MAPS_TO_VALUE;
               } else if (strcmp(maps_str, "action") == 0) {
                  param->maps_to = CMD_MAPS_TO_ACTION;
               } else if (strcmp(maps_str, "device") == 0) {
                  param->maps_to = CMD_MAPS_TO_DEVICE;
               } else {
                  param->maps_to = CMD_MAPS_TO_CUSTOM;
                  safe_strncpy(param->field_name, maps_str, CMD_NAME_LEN);
               }
            } else {
               param->maps_to = CMD_MAPS_TO_VALUE;
            }

            cmd->param_count++;
         }
      }

      json_object_iter_next(&it);
   }

   return 0;
}

/**
 * @brief Load and parse the commands config file
 */
static int load_config_file(const char *path) {
   FILE *fp = fopen(path, "r");
   if (!fp) {
      LOG_ERROR("command_registry: Failed to open config file: %s", path);
      return 1;
   }

   /* Read entire file */
   fseek(fp, 0, SEEK_END);
   long file_size = ftell(fp);
   fseek(fp, 0, SEEK_SET);

   char *buffer = malloc(file_size + 1);
   if (!buffer) {
      LOG_ERROR("command_registry: Failed to allocate buffer for config file");
      fclose(fp);
      return 1;
   }

   size_t bytes_read = fread(buffer, 1, file_size, fp);
   buffer[bytes_read] = '\0';
   fclose(fp);

   /* Parse JSON */
   struct json_object *root = json_tokener_parse(buffer);
   free(buffer);

   if (!root) {
      LOG_ERROR("command_registry: Failed to parse JSON config");
      return 1;
   }

   /* Parse devices section */
   struct json_object *devices_obj = NULL;
   if (json_object_object_get_ex(root, "devices", &devices_obj)) {
      parse_devices(devices_obj);
   }

   /* Parse tools section (meta-tools) */
   struct json_object *tools_obj = NULL;
   if (json_object_object_get_ex(root, "tools", &tools_obj)) {
      parse_tools(tools_obj);
   }

   json_object_put(root);
   return 0;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int command_registry_init(void) {
   if (s_initialized) {
      LOG_WARNING("command_registry: Already initialized");
      return 0;
   }

   s_command_count = 0;
   s_alias_count = 0;
   memset(s_commands, 0, sizeof(s_commands));
   memset(s_aliases, 0, sizeof(s_aliases));
   hash_init(); /* Initialize hash tables for O(1) lookups */

   /* Load from config file */
   const char *config_path = g_config.paths.commands_config;
   if (!config_path || config_path[0] == '\0') {
      config_path = "commands_config_nuevo.json";
   }

   /* Security: reject path traversal attempts */
   if (strstr(config_path, "..") != NULL) {
      LOG_ERROR("command_registry: Path traversal rejected in config path: %s", config_path);
      return 1;
   }

   if (load_config_file(config_path) != 0) {
      LOG_ERROR("command_registry: Failed to load config file");
      return 1;
   }

   /* Cache the enabled count */
   s_enabled_count = 0;
   for (int i = 0; i < s_command_count; i++) {
      if (s_commands[i].enabled) {
         s_enabled_count++;
      }
   }

   s_initialized = true;
   LOG_INFO("command_registry: Initialized with %d commands (%d enabled), %d aliases",
            s_command_count, s_enabled_count, s_alias_count);

   return 0;
}

void command_registry_shutdown(void) {
   s_command_count = 0;
   s_enabled_count = 0;
   s_alias_count = 0;
   s_initialized = false;
   LOG_INFO("command_registry: Shutdown complete");
}

const cmd_definition_t *command_registry_lookup(const char *name) {
   if (!name || !s_initialized) {
      return NULL;
   }

   /* First, try direct name match */
   int idx = find_command_index(name);
   if (idx >= 0) {
      return &s_commands[idx];
   }

   /* Then, try alias lookup */
   idx = find_by_alias(name);
   if (idx >= 0) {
      return &s_commands[idx];
   }

   return NULL;
}

bool command_registry_validate(const char *device, char *topic_out, size_t topic_size) {
   const cmd_definition_t *cmd = command_registry_lookup(device);
   if (!cmd) {
      return false;
   }

   if (topic_out && topic_size > 0) {
      safe_strncpy(topic_out, cmd->topic, topic_size);
   }

   return true;
}

int command_registry_count(void) {
   return s_command_count;
}

int command_registry_enabled_count(void) {
   return s_enabled_count;
}

void command_registry_foreach(cmd_foreach_callback_t callback, void *user_data) {
   if (!callback) {
      return;
   }

   for (int i = 0; i < s_command_count; i++) {
      callback(&s_commands[i], user_data);
   }
}

void command_registry_foreach_enabled(cmd_foreach_callback_t callback, void *user_data) {
   if (!callback) {
      return;
   }

   for (int i = 0; i < s_command_count; i++) {
      if (s_commands[i].enabled) {
         callback(&s_commands[i], user_data);
      }
   }
}

const char *command_registry_resolve_device(const cmd_definition_t *cmd, const char *key) {
   if (!cmd || !key || cmd->device_map_count == 0) {
      return NULL;
   }

   for (int i = 0; i < cmd->device_map_count; i++) {
      if (strcmp(cmd->device_map[i].key, key) == 0) {
         return cmd->device_map[i].device;
      }
   }

   return NULL;
}
