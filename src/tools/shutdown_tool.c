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
 * Shutdown Tool Implementation
 */

#include "tools/shutdown_tool.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "logging.h"
#include "tools/tool_registry.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Constant-Time String Comparison (Security)
 * ============================================================================= */

/**
 * @brief Constant-time string comparison
 *
 * Compares two strings in constant time to prevent timing attacks.
 * The comparison takes the same time regardless of where strings differ.
 *
 * @param a First string
 * @param b Second string
 * @return 0 if strings are equal, non-zero otherwise
 */
static int constant_time_compare(const char *a, const char *b) {
   if (!a || !b) {
      return 1; /* Not equal if either is NULL */
   }

   size_t len_a = strlen(a);
   size_t len_b = strlen(b);

   /* Use volatile to prevent compiler optimizations */
   volatile int result = (int)(len_a ^ len_b);

   /* Compare all characters - don't short-circuit */
   size_t min_len = (len_a < len_b) ? len_a : len_b;
   for (size_t i = 0; i < min_len; i++) {
      result |= ((unsigned char)a[i]) ^ ((unsigned char)b[i]);
   }

   return result;
}

/* A spoken passphrase has no punctuation, and ASCII letters no case: compare it
 * folded (ASCII letters and digits lowercased, other UTF-8 bytes kept as they
 * are, so non-ASCII letters stay case-sensitive; anything else one space,
 * trimmed) */
static void fold_spoken(const char *in, char *out, size_t size) {
   size_t n = 0;
   bool gap = false;
   for (const char *p = in; *p && n + 1 < size; p++) {
      unsigned char c = (unsigned char)*p;
      if (isalnum(c) || c >= 0x80) {
         if (gap && n > 0 && n + 2 < size) {
            out[n++] = ' ';
         }
         out[n++] = c < 0x80 ? (char)tolower(c) : (char)c;
         gap = false;
      } else {
         gap = true;
      }
   }
   out[n] = '\0';
}

/* Whether the voice shortcut ("shut down <passphrase>") is offered: only when
 * shutdown is enabled and a passphrase is set, since the shortcut exists to
 * carry the passphrase.  Otherwise "shut down the TV" goes on to the LLM. */
static bool shutdown_is_available(void) {
   pthread_rwlock_rdlock(&g_config_rwlock);
   const shutdown_config_t *cfg = &config_get()->shutdown;
   const bool available = cfg->enabled && cfg->passphrase[0] != '\0';
   pthread_rwlock_unlock(&g_config_rwlock);
   return available;
}

/* =============================================================================
 * Tool Callback
 * ============================================================================= */

/**
 * @brief Shutdown command callback
 *
 * Executes system shutdown if security checks pass:
 * 1. [shutdown] enabled in the config
 * 2. Passphrase must match (if configured)
 *
 * The settings live in the global config ([shutdown], g_config.shutdown), which
 * the settings panel edits.  `enabled` is checked here because this callback is
 * reached by voice commands and MQTT as well as the model.
 *
 * @param action Action name (unused)
 * @param value Passphrase from user (if passphrase required)
 * @param should_respond Set to 1 to return result to LLM
 * @return Response string (caller must free), or NULL
 */
static char *shutdown_tool_callback(const char *action, char *value, int *should_respond) {
   (void)action; /* Unused */
   *should_respond = 1;

   /* One consistent copy: the settings panel may be saving them now. */
   pthread_rwlock_rdlock(&g_config_rwlock);
   const shutdown_config_t *cfg = &config_get()->shutdown;
   const bool enabled = cfg->enabled;
   char passphrase[sizeof(cfg->passphrase)];
   safe_strscpy(passphrase, cfg->passphrase);
   pthread_rwlock_unlock(&g_config_rwlock);

   /* Security check 1: Must be explicitly enabled in config */
   if (!enabled) {
      OLOG_WARNING("Shutdown command rejected: shutdown.enabled = false in config");
      return strdup(TOOL_RESULT_ERROR_MARK
                    "Shutdown command is disabled. Enable it in settings first.");
   }

   /* Security check 2: If passphrase is configured, it must match (folded: the
    * voice shortcut hands it over lowercased, with Whisper's punctuation) */
   if (passphrase[0] != '\0') {
      char want[sizeof(passphrase)] = { 0 };
      char got[sizeof(passphrase) + 1] = { 0 }; /* longer than any passphrase: extra words differ */
      fold_spoken(passphrase, want, sizeof(want));
      fold_spoken(value ? value : "", got, sizeof(got));
      if (want[0] == '\0') {
         /* Nothing left to compare: an empty match must not authorize */
         OLOG_ERROR("Shutdown command rejected: the passphrase has no letters or digits");
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Shutdown command rejected: the configured passphrase can't be spoken.");
      }
      if (constant_time_compare(got, want) != 0) {
         OLOG_WARNING("Shutdown command rejected: incorrect or missing passphrase");
         return strdup(TOOL_RESULT_ERROR_MARK "Shutdown command rejected: incorrect passphrase.");
      }
      OLOG_INFO("Shutdown passphrase verified");
   }

   /* All security checks passed - execute shutdown */
   OLOG_INFO("Shutdown command authorized, initiating system shutdown");

   /* constant command; runs only when enabled and after the passphrase check */
   // NOLINTNEXTLINE(cert-env33-c)
   int ret = system("sudo shutdown -h now");
   if (ret != 0) {
      OLOG_ERROR("Shutdown command failed with return code: %d", ret);
      return strdup(TOOL_RESULT_ERROR_MARK "Shutdown command failed to execute.");
   }

   return strdup("Shutdown authorized. Initiating system shutdown. Goodbye.");
}

/* =============================================================================
 * Tool Metadata
 * ============================================================================= */

/* Parameter definitions */
static const treg_param_t shutdown_params[] = {
   {
       .name = "passphrase",
       .description = "Security passphrase if configured (optional)",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

/* Tool metadata - static lifetime required */
static const tool_metadata_t shutdown_metadata = {
   /* Identity */
   .name = "shutdown",
   /* Kind of action: act (the default): stops the daemon. */
   .device_string = "shutdown",
   .topic = "dawn",
   /* Whisper often writes "shut down" as two words */
   .aliases = { "shut down" },
   .alias_count = 1,

   /* LLM Tool Schema */
   .description = "Initiate system shutdown. Requires explicit config enable "
                  "and optional passphrase for security.",
   .params = shutdown_params,
   .param_count = TOOL_PARAM_COUNT(shutdown_params),

   /* Device Mapping */
   .device_map = NULL,
   .device_map_count = 0,

   /* Behavior Flags */
   .device_type = TOOL_DEVICE_TYPE_PASSPHRASE,
   .capabilities = TOOL_CAP_DANGEROUS,
   .skip_followup = false,
   .mqtt_only = false,
   .sync_wait = false,
   .default_local = true,
   .default_remote = false, /* Security: no remote shutdown */


   /* Secret Requirements */
   .secret_requirements = NULL, /* No secrets needed */

   /* Lifecycle */
   .init = NULL,    /* No special init needed */
   .cleanup = NULL, /* No cleanup needed */

   /* Callback */
   .is_available = shutdown_is_available,
   .callback = shutdown_tool_callback,
};

/* =============================================================================
 * Registration
 * ============================================================================= */

int shutdown_tool_register(void) {
   return tool_registry_register(&shutdown_metadata);
}
