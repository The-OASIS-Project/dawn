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
 * Device Types - Compile-time definitions for direct command pattern matching
 */

#include "core/device_types.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

/* ========== Boolean Device Type ========== */

const device_type_def_t DEVICE_TYPE_BOOLEAN = {
   .name = "boolean",
   .actions = {
      {
         .name = "enable",
         .patterns = {
            "enable %device_name%",
            "turn on %device_name%",
            "switch on %device_name%",
            "show %device_name%",
            "display %device_name%",
            "open %device_name%",
            "start %device_name%",
            "switch to %device_name%",
         },
         .pattern_count = 8,
      },
      {
         .name = "disable",
         .patterns = {
            "disable %device_name%",
            "turn off %device_name%",
            "switch off %device_name%",
            "hide %device_name%",
            "close %device_name%",
            "stop %device_name%",
         },
         .pattern_count = 6,
      },
   },
   .action_count = 2,
};

/* ========== Analog Device Type ========== */

const device_type_def_t DEVICE_TYPE_ANALOG = {
   .name = "analog",
   .actions = {
      {
         .name = "set",
         .patterns = {
            "set %device_name% to %value%",
            "adjust %device_name% to %value%",
            "switch %device_name% to %value%",
            "switch to %value% %device_name%",
         },
         .pattern_count = 4,
      },
   },
   .action_count = 1,
};

/* ========== Getter Device Type ========== */

const device_type_def_t DEVICE_TYPE_GETTER = {
   .name = "getter",
   .actions = {
      {
         .name = "get",
         .patterns = {
            "what is the %device_name%",
            "what %device_name% is it",
            "give me the %device_name%",
            "tell me the %device_name%",
            "what is %device_name%",
            "what's %device_name%",
            "what am i %device_name%",
         },
         .pattern_count = 7,
      },
   },
   .action_count = 1,
};

/* ========== Music Device Type ========== */

const device_type_def_t DEVICE_TYPE_MUSIC = {
   .name = "music",
   .actions = {
      {
         .name = "next",
         .patterns = {
            "next song",
            "next track",
            "play next song",
            "play next track",
            "play next",
         },
         .pattern_count = 5,
      },
      {
         .name = "previous",
         .patterns = {
            "previous song",
            "previous track",
            "play previous song",
            "play previous track",
            "play previous",
         },
         .pattern_count = 5,
      },
      {
         .name = "play",
         .patterns = {
            "play %value%",
            "play music %value%",
            "play me some %value%",
            "can you play %value%",
         },
         .pattern_count = 4,
      },
      {
         .name = "stop",
         .patterns = {
            "stop music",
            "stop playing music",
            "stop playback",
            "stop the music",
         },
         .pattern_count = 4,
      },
   },
   .action_count = 4,
};

/* ========== Trigger Device Type ========== */

const device_type_def_t DEVICE_TYPE_TRIGGER = {
   .name = "trigger",
   .actions = {
      {
         .name = "trigger",
         .patterns = {
            "%device_name%",
            "please %device_name%",
            "can you %device_name%",
         },
         .pattern_count = 3,
      },
   },
   .action_count = 1,
};

/* ========== Passphrase Trigger Device Type ========== */

const device_type_def_t DEVICE_TYPE_PASSPHRASE = {
   .name = "passphrase_trigger",
   .actions = {
      {
         .name = "execute",
         /* The generic "%device_name% %value%" last: it would also catch the
          * phrased forms, with their words in the value.  Whisper writes
          * "passphrase" both ways. */
         .patterns = {
            "%device_name% with passphrase %value%",
            "%device_name% with pass phrase %value%",
            "%device_name% authorization %value%",
            "execute %device_name% %value%",
            "%device_name% %value%",
         },
         .pattern_count = 5,
      },
   },
   .action_count = 1,
};

/* ========== Lookup Functions ========== */

const device_type_def_t *device_type_get_def(tool_device_type_t type) {
   switch (type) {
      case TOOL_DEVICE_TYPE_BOOLEAN:
         return &DEVICE_TYPE_BOOLEAN;
      case TOOL_DEVICE_TYPE_ANALOG:
         return &DEVICE_TYPE_ANALOG;
      case TOOL_DEVICE_TYPE_GETTER:
         return &DEVICE_TYPE_GETTER;
      case TOOL_DEVICE_TYPE_MUSIC:
         return &DEVICE_TYPE_MUSIC;
      case TOOL_DEVICE_TYPE_TRIGGER:
         return &DEVICE_TYPE_TRIGGER;
      case TOOL_DEVICE_TYPE_PASSPHRASE:
         return &DEVICE_TYPE_PASSPHRASE;
      default:
         return NULL;
   }
}

const char *device_type_get_name(tool_device_type_t type) {
   const device_type_def_t *def = device_type_get_def(type);
   return def ? def->name : "unknown";
}

bool device_type_value_is_secret(tool_device_type_t type) {
   return type == TOOL_DEVICE_TYPE_PASSPHRASE;
}

/* ========== Pattern Matching ========== */

/**
 * @brief Case-insensitive string prefix match
 *
 * @return Length matched if prefix matches, 0 otherwise
 */
static size_t match_prefix_ci(const char *input, const char *prefix) {
   size_t len = 0;
   while (*prefix && *input) {
      if (tolower((unsigned char)*input) != tolower((unsigned char)*prefix)) {
         return 0;
      }
      input++;
      prefix++;
      len++;
   }
   /* Prefix must be fully consumed */
   if (*prefix) {
      return 0;
   }
   return len;
}

/**
 * @brief Check if input matches device name or any alias (case-insensitive)
 *
 * @return Length of matched name, 0 if no match
 */
static size_t match_device_name(const char *input, const char *device_name, const char **aliases) {
   size_t len;

   /* Try primary device name */
   len = match_prefix_ci(input, device_name);
   if (len > 0) {
      return len;
   }

   /* Try aliases */
   if (aliases) {
      for (int i = 0; aliases[i] != NULL; i++) {
         len = match_prefix_ci(input, aliases[i]);
         if (len > 0) {
            return len;
         }
      }
   }

   return 0;
}

/**
 * @brief Skip whitespace in input
 */
static const char *skip_whitespace(const char *p) {
   while (*p && isspace((unsigned char)*p)) {
      p++;
   }
   return p;
}

/**
 * @brief Match a single pattern against input
 *
 * Handles %device_name% and %value% placeholders.
 * Case-insensitive matching for literal parts.
 *
 * @return true if pattern matches, false otherwise
 */
static bool match_single_pattern(const char *pattern,
                                 const char *input,
                                 const char *device_name,
                                 const char **aliases,
                                 char *out_value,
                                 size_t value_size) {
   const char *p = pattern;
   const char *i = input;

   /* Skip leading whitespace in input */
   i = skip_whitespace(i);

   while (*p) {
      if (strncmp(p, "%device_name%", 13) == 0) {
         /* Match device name or alias */
         size_t name_len = match_device_name(i, device_name, aliases);
         if (name_len == 0) {
            return false;
         }
         i += name_len;
         p += 13;

         /* After device name, require a word boundary if more pattern follows:
          * whitespace, punctuation ("Shutdown, alpha bravo" as Whisper writes
          * it), or the end */
         while (*p && ispunct((unsigned char)*i)) {
            i++;
         }
         if (*p && !isspace((unsigned char)*i) && *i != '\0') {
            return false;
         }
      } else if (strncmp(p, "%value%", 7) == 0) {
         /* Capture remaining input as value (trimmed) */
         i = skip_whitespace(i);
         if (out_value && value_size > 0) {
            /* Copy rest of input as value, trim trailing whitespace */
            size_t len = strlen(i);
            while (len > 0 && isspace((unsigned char)i[len - 1])) {
               len--;
            }
            if (len >= value_size) {
               len = value_size - 1;
            }
            memcpy(out_value, i, len);
            out_value[len] = '\0';
         }
         /* Value consumes rest of input, so pattern must end after %value% */
         p += 7;
         if (*p) {
            /* More pattern after %value% - not supported in current patterns */
            return false;
         }
         return true;
      } else if (isspace((unsigned char)*p)) {
         /* Match whitespace: pattern space matches one or more input spaces */
         if (!isspace((unsigned char)*i)) {
            return false;
         }
         while (isspace((unsigned char)*p)) {
            p++;
         }
         while (isspace((unsigned char)*i)) {
            i++;
         }
      } else {
         /* Match literal character (case-insensitive) */
         if (tolower((unsigned char)*p) != tolower((unsigned char)*i)) {
            return false;
         }
         p++;
         i++;
      }
   }

   /* Pattern fully consumed - input should be at end or only whitespace remains */
   i = skip_whitespace(i);
   return (*i == '\0');
}

bool device_type_match_pattern(const device_type_def_t *type,
                               const char *input,
                               const char *device_name,
                               const char **aliases,
                               const char **out_action,
                               char *out_value,
                               size_t value_size) {
   if (!type || !input || !device_name) {
      return false;
   }

   /* Clear output value */
   if (out_value && value_size > 0) {
      out_value[0] = '\0';
   }

   /* Try each action */
   for (int a = 0; a < type->action_count; a++) {
      const device_action_def_t *action = &type->actions[a];

      /* Try each pattern for this action */
      for (int p = 0; p < action->pattern_count; p++) {
         if (match_single_pattern(action->patterns[p], input, device_name, aliases, out_value,
                                  value_size)) {
            if (out_action) {
               *out_action = action->name;
            }
            return true;
         }
      }
   }

   return false;
}
