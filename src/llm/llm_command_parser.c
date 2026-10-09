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
 */

/* Std C */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Local */
#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "dawn.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_interface.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "prompts.h"
#include "tools/tool_registry.h"

/* The prompt text itself lives in prompts.h (SYSTEM_PROMPT_*).  Tool use is
 * native function calling: SYSTEM_PROMPT_RESPONSE_RULES apply always, and
 * SYSTEM_PROMPT_NATIVE_TOOLS_RULES follow them when tools are enabled.  See
 * get_system_instructions() for the branching logic. */

// Static buffer for the command prompt - make it static, make it large
#define PROMPT_BUFFER_SIZE 65536
static char command_prompt[PROMPT_BUFFER_SIZE];
static int prompt_initialized = 0;

// Static buffer for localization context
#define LOCALIZATION_BUFFER_SIZE 512
static char localization_context[LOCALIZATION_BUFFER_SIZE];
static int localization_initialized = 0;

// Static buffer for dynamic system instructions.
#define SYSTEM_INSTRUCTIONS_BUFFER_SIZE 8192
static char system_instructions_buffer[SYSTEM_INSTRUCTIONS_BUFFER_SIZE];
static int system_instructions_initialized = 0;

/*
 * Serializes all access to the cached system_instructions state above, plus
 * the derived command_prompt buffer and its _initialized flag. Before this existed, invalidation
 * was called only at well-defined init boundaries and the buffers were treated as build-once; now
 * invalidation fires from MQTT callback threads (HUD status / discovery) while LLM worker threads
 * may be mid-read, so the previous lock-free pattern no longer holds.
 */
static pthread_mutex_t system_instructions_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Checks if vision is enabled for the current LLM type
 *
 * Vision availability is controlled by the vision_enabled setting for the
 * session's LLM type (cloud or local). This ensures that when the user
 * switches LLMs at runtime, the vision check reflects the session's config.
 *
 * @return 1 if vision is available, 0 otherwise
 */
int is_vision_enabled_for_current_llm(void) {
   /* Check session context first (set during streaming calls) */
   session_t *session = session_get_command_context();
   if (session) {
      session_llm_config_t config;
      session_get_llm_config(session, &config);
      if (config.type == LLM_CLOUD) {
         return g_config.llm.cloud.vision_enabled;
      } else if (config.type == LLM_LOCAL) {
         return g_config.llm.local.vision_enabled;
      }
      return 0;
   }

   /* Fallback to global state for paths without session context */
   llm_type_t current = llm_get_type();
   if (current == LLM_CLOUD) {
      return g_config.llm.cloud.vision_enabled;
   } else if (current == LLM_LOCAL) {
      return g_config.llm.local.vision_enabled;
   }
   return 0;
}


/**
 * @brief Invalidates cached system instructions, forcing rebuild on next call
 *
 * Call this when capabilities change at runtime (e.g., a tool's auth
 * status changes, devices are loaded, etc.) so the next call to
 * get_system_instructions() rebuilds the prompt with updated capabilities.
 */
void invalidate_system_instructions(void) {
   pthread_mutex_lock(&system_instructions_mutex);
   system_instructions_initialized = 0;
   prompt_initialized = 0;
   pthread_mutex_unlock(&system_instructions_mutex);
   OLOG_INFO("System instructions cache invalidated - will rebuild on next LLM call");
}

/**
 * @brief Truncation-safe formatted append into a fixed buffer
 *
 * snprintf returns the length it WOULD have written, so `len += snprintf(...)`
 * can advance past the buffer when the content is truncated — after which
 * `buffer + len` / `size - len` go out of bounds. This clamps: it writes at
 * buffer[*len], never exceeds cap-1 bytes, and advances *len only by the bytes
 * actually written.
 */
static void instr_appendf(char *buffer, int cap, int *len, const char *fmt, ...) {
   if (!buffer || *len < 0 || *len >= cap - 1) {
      return;
   }
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(buffer + *len, (size_t)(cap - *len), fmt, ap);
   va_end(ap);
   if (n < 0) {
      return;
   }
   *len += (n < cap - *len) ? n : (cap - *len - 1);
}

/**
 * @brief Build system instructions into a provided buffer
 *
 * @param tools_on true = native tool rules; false = prose format rules only
 * @param buffer Output buffer to write instructions to
 * @param buffer_size Size of the output buffer
 * @return Number of bytes written (excluding null terminator)
 */
static int build_system_instructions_to_buffer(bool tools_on, char *buffer, size_t buffer_size) {
   int len = 0;
   int cap = (int)buffer_size;

   /* The reply rules first: they apply whether or not tools are enabled. */
   instr_appendf(buffer, cap, &len, "%s", SYSTEM_PROMPT_RESPONSE_RULES);

   /* Tools off = the reply rules only; tools on = the tool rules after them. */
   if (!tools_on) {
      return len;
   }

   instr_appendf(buffer, cap, &len, "%s\n", SYSTEM_PROMPT_NATIVE_TOOLS_RULES);
   /* The plan executor's DSL when the tool is registered (with enough tools
    * for a plan to use): by registration, like a conversation's frozen tool
    * set, never by what is enabled now, so the prompt doesn't change as tools
    * are switched on and off (that reaches the model as a direction). */
   if (tool_registry_find("execute_plan") != NULL && tool_registry_count() >= 3) {
      instr_appendf(buffer, cap, &len, "%s", SYSTEM_PROMPT_PLAN_EXECUTOR);
   }
   /* Which tools are unavailable right now is not here: it changes as devices
    * come and go, and a conversation's system prompt must not.  It reaches the
    * model with the turn's standing directions (dawn_build_prompt). */
   return len;
}

const char *get_system_instructions(void) {
   pthread_mutex_lock(&system_instructions_mutex);

   if (system_instructions_initialized) {
      pthread_mutex_unlock(&system_instructions_mutex);
      return system_instructions_buffer;
   }

   /* Tools on/off from global config (native tool calling is the only tool path). */
   bool tools_on = llm_tools_enabled(NULL);

   int len = build_system_instructions_to_buffer(tools_on, system_instructions_buffer,
                                                 SYSTEM_INSTRUCTIONS_BUFFER_SIZE);

   system_instructions_initialized = 1;

   pthread_mutex_unlock(&system_instructions_mutex);

   OLOG_INFO("Built system instructions (%s, %d bytes)", tools_on ? "tools on" : "tools off", len);

   return system_instructions_buffer;
}

/**
 * @brief Builds the localization context string from config
 *
 * Creates a context string like:
 * "USER CONTEXT: Location: Atlanta, Georgia. Units: imperial. Timezone: America/New_York."
 *
 * Only includes fields that are configured (non-empty).
 */
static const char *get_localization_context(void) {
   if (localization_initialized) {
      return localization_context;
   }

   localization_context[0] = '\0';
   int offset = 0;
   int has_context = 0;

   // Check if any localization fields are set
   /* Where the daemon itself sits (its room) is not here: the prompt is every
    * surface's, and a room is one surface's (a local standing direction). */
   if (g_config.localization.location[0] != '\0' || g_config.localization.units[0] != '\0' ||
       g_config.localization.timezone[0] != '\0') {
      offset = snprintf(localization_context, LOCALIZATION_BUFFER_SIZE, "%s",
                        TOOL_DEFAULTS_HEADER_TEXT);
      has_context = 1;
   }

   if (g_config.localization.location[0] != '\0') {
      offset += snprintf(localization_context + offset, LOCALIZATION_BUFFER_SIZE - offset,
                         " Location=%s.", g_config.localization.location);
   }

   if (g_config.localization.units[0] != '\0') {
      offset += snprintf(localization_context + offset, LOCALIZATION_BUFFER_SIZE - offset,
                         " Units=%s.", g_config.localization.units);
   }

   if (g_config.localization.timezone[0] != '\0') {
      offset += snprintf(localization_context + offset, LOCALIZATION_BUFFER_SIZE - offset,
                         " TZ=%s.", g_config.localization.timezone);
   }

   /* No `Today=`: the time is in each turn's context ([system_time]).  A date
    * here would change the prompt at midnight, and a conversation's prompt is
    * frozen. */

   if (has_context) {
      snprintf(localization_context + offset, LOCALIZATION_BUFFER_SIZE - offset, "\n\n");
   }

   localization_initialized = 1;
   return localization_context;
}

void llm_persona_default(char *out, size_t size) {
   const char *ai_name = g_config.general.ai_name[0] != '\0' ? g_config.general.ai_name : AI_NAME;

   /* Capitalized: it's a name. */
   char name[64];
   snprintf(name, sizeof(name), "%s", ai_name);
   if (name[0] >= 'a' && name[0] <= 'z') {
      name[0] -= 32;
   }
   snprintf(out, size, AI_PERSONA_TEMPLATE, name);
}

/* The persona the prompt opens with: the configured [persona] description,
 * else the default.  Built on each call, never cached, so a rename or a new
 * description reaches the next prompt built. */
static void persona_effective(char *out, size_t size) {
   if (g_config.persona.description[0] != '\0') {
      snprintf(out, size, "%s", g_config.persona.description);
   } else {
      llm_persona_default(out, size);
   }
}

/**
 * @brief Builds the command prompt every surface starts from
 *
 * Persona, system instructions and localization.  Nothing about the surface a
 * turn arrives on (a local mic's voice and room, a satellite's room, a
 * channel): those are standing directions (dawn_build_prompt), so a
 * conversation that moves between surfaces keeps one system prompt.
 */
static void initialize_command_prompt(void) {
   /* Gather inputs without holding the mutex — these call helpers that each
    * take the mutex briefly (get_system_instructions) or none at all. */
   char persona[CONFIG_DESCRIPTION_MAX];
   persona_effective(persona, sizeof(persona));
   const char *sys_instr = get_system_instructions();
   const char *loc_ctx = get_localization_context();

   pthread_mutex_lock(&system_instructions_mutex);
   if (prompt_initialized) {
      pthread_mutex_unlock(&system_instructions_mutex);
      return;
   }

   int prompt_len = snprintf(command_prompt, PROMPT_BUFFER_SIZE, "%s\n\n%s\n\n%s", persona,
                             sys_instr, loc_ctx);
   prompt_initialized = 1;
   pthread_mutex_unlock(&system_instructions_mutex);

   OLOG_INFO("AI prompt initialized (tools %s). Length: %d",
             g_config.llm.tools.enabled ? "on" : "off", prompt_len);
}

int get_command_prompt_parts(command_prompt_parts_t *out) {
   if (!out) {
      return 1;
   }
   memset(out, 0, sizeof(*out));
   char persona[CONFIG_DESCRIPTION_MAX];
   persona_effective(persona, sizeof(persona));
   (void)get_system_instructions();
   (void)get_localization_context();
   /* Copied under the mutex a rebuild writes the buffers under. */
   pthread_mutex_lock(&system_instructions_mutex);
   out->persona = strdup(persona);
   out->rules = strdup(system_instructions_buffer);
   out->tool_defaults = strdup(localization_context);
   pthread_mutex_unlock(&system_instructions_mutex);
   if (!out->persona || !out->rules || !out->tool_defaults) {
      command_prompt_parts_free(out);
      return 1;
   }
   return 0;
}

void command_prompt_parts_free(command_prompt_parts_t *parts) {
   if (!parts) {
      return;
   }
   free(parts->persona);
   free(parts->rules);
   free(parts->tool_defaults);
   memset(parts, 0, sizeof(*parts));
}

char *get_command_prompt_dup(void) {
   initialize_command_prompt();
   /* Copied under the mutex a rebuild writes the buffer under, so a config edit
    * mid-copy can't hand the caller half of each. */
   pthread_mutex_lock(&system_instructions_mutex);
   char *copy = strdup(command_prompt);
   pthread_mutex_unlock(&system_instructions_mutex);
   return copy;
}

/* =============================================================================
 * Voice-session prompt directives — effective-value accessors
 *
 * Config field set → use it; empty → fall back to the compile-time default.
 * See the header contract and dawn.h for the built-in text.
 *
 * Concurrency note: these return a pointer directly into g_config and are read
 * unlocked on the prompt-build path — consistent with every other g_config read
 * there (persona, llm.tools.enabled, localization, ...).  A WebUI config save
 * (handle_set_config, under s_config_rwlock wrlock) racing an in-flight turn
 * could yield a torn directive for that one turn.  The buffers are fixed-size
 * (CONFIG_DESCRIPTION_MAX) and strncpy-padded, so a torn read is still a bounded,
 * NUL-terminated string (no over-read / memory unsafety) — worst case is one
 * turn's directive being garbled.  Not spot-locked here on purpose: locking only
 * these three fields while the rest of the prompt-build g_config reads stay
 * unlocked would be inconsistent and give false assurance.  A proper fix is a
 * broader prompt-build config snapshot, tracked separately.
 * ============================================================================= */
const char *voice_directive_effective(void) {
   return g_config.tts.voice_directive[0] ? g_config.tts.voice_directive
                                          : DEFAULT_VOICE_OUTPUT_DIRECTIVE;
}

const char *voice_directive_webui_effective(void) {
   return g_config.tts.voice_directive_webui[0] ? g_config.tts.voice_directive_webui
                                                : DEFAULT_VOICE_OUTPUT_DIRECTIVE_WEBUI;
}

const char *asr_disambiguation_hint_effective(void) {
   return g_config.asr.disambiguation_hint[0] ? g_config.asr.disambiguation_hint
                                              : DEFAULT_ASR_DISAMBIGUATION_HINT;
}
