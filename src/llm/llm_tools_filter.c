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
 * the project author(s).
 *
 * Which tools a request advertises: what a surface may use now
 * (llm_tools_enabled_for_session), a conversation's tools by value
 * (llm_tool_defs.h), the research allowlist; the registry's definitions and
 * their schemas; and the standing direction naming the tools not available
 * now.  Split from llm_tools.c (shared state:
 * llm_tools_internal.h).
 */

#include <json-c/json.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/hash_util.h"
#include "core/research_allowlist.h"
#include "core/session_manager.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_tool_defs.h"
#include "llm/llm_tools.h"
#include "llm/llm_tools_internal.h"
#include "logging.h"
#include "tools/tool_registry.h"

/* Thread-local suppression counter for temporarily disabling tools.
 * Used by subsystems like the search summarizer that need to make
 * LLM calls without tools being added to the request. */
static __thread int tl_suppress_count = 0;

void llm_tools_suppress_push(void) {
   tl_suppress_count++;
}

void llm_tools_suppress_pop(void) {
   if (tl_suppress_count > 0) {
      tl_suppress_count--;
   }
}

bool llm_tools_suppressed(void) {
   return tl_suppress_count > 0;
}

/* The deep-research fetch loop runs on a read-only tool allowlist: while a
 * research session is active, ONLY these tools are reachable — no email, HA,
 * phone, shutdown, or any other side-effecting verb (DEEP_RESEARCH_DESIGN §7/§11
 * plan HIGH-1).  research_plan/research_record are ALSO research-only: hidden
 * from every non-research session.  The name list is single-sourced in
 * core/research_allowlist.h so the native gate here and the command_execute
 * defense-in-depth close (HIGH-1) cannot drift. */

/**
 * @brief Check if a tool is enabled for a given session type
 */
bool llm_tools_enabled_for_session(const tool_definition_t *t, bool is_remote) {
   if (!t->enabled) {
      return false; /* Capability not available */
   }

   /* Research read-only allowlist (enforced at schema advertisement AND
    * execution: the *_format_filtered() schema builders and llm_tools_execute()
    * both route through this one function).  A conversation's frozen tool set
    * (llm_tools_request_tools) advertises tools this doesn't allow here; they
    * are refused at execution, here, and the turn's standing directions say so
    * (llm_tools_build_disabled_hint).  When the command-context session is a research run, ONLY the
    * allowlisted tools are visible/executable; when it is NOT, the two
    * research-only tools are hidden.  t->enabled is still honored (a globally-
    * disabled web tool stays off even for research).
    *
    * NOTE: this gates only the NATIVE tool path.  The legacy <command>-tag path
    * (command_execute) does NOT consult this, so it is NOT closed here — the
    * research fetch loop must run native-tools-only, and command_execute needs a
    * research-aware refusal as defense-in-depth (DEEP_RESEARCH_DESIGN §11 HIGH-1).
    * Both are the research_worker's session-setup responsibility (Step 6), with a
    * regression test that a research-context command_execute is refused. */
   session_t *ctx = session_get_command_context();
   /* No-tools turn (e.g. the deep-research synthesis turn): deny EVERY tool so the
    * turn is pure text.  Checked before the allowlist so it also suppresses the read
    * tools. */
   if (ctx != NULL && session_tools_suppressed(ctx)) {
      return false;
   }
   bool research_mode = (ctx != NULL && atomic_load(&ctx->research_run_id) > 0);
   if (research_mode) {
      return research_tool_is_allowlisted(t->name);
   }
   if (research_tool_is_research_only(t->name)) {
      return false; /* research_plan/research_record never appear outside a research session */
   }

   /* Headless background-job workers must not fan out into more jobs — hide the
    * job-spawn tool from a SESSION_TYPE_JOB context's schema so the model never
    * sees it.  (handle_spawn also hard-refuses a job-context caller as a backstop
    * for any non-schema path, e.g. a legacy <command> tag.) */
   if (strcmp(t->name, "job") == 0) {
      if (ctx != NULL && ctx->type == SESSION_TYPE_JOB) {
         return false;
      }
   }
   return is_remote ? t->enabled_remote : t->enabled_local;
}

/* A tool's neutral definition (llm_tool_defs.h): what the registry says now. */
static struct json_object *neutral_def(const tool_definition_t *t) {
   struct json_object *def = json_object_new_object();
   if (def) {
      json_object_object_add(def, "name", json_object_new_string(t->name));
      json_object_object_add(def, "description",
                             json_object_new_string(llm_tools_effective_description(t)));
      json_object_object_add(def, "parameters", llm_tools_parameters_schema(t));
   }
   return def;
}

/* One tool's schema as the registry renders it now. */
static struct json_object *tool_schema(const tool_definition_t *t, bool claude) {
   struct json_object *def = neutral_def(t);
   struct json_object *out = def ? llm_tool_def_render(def, claude) : NULL;
   json_object_put(def);
   return out;
}

static struct json_object *format_filtered(bool is_remote_session, bool claude) {
   if (!llm_tools_ready || llm_tools_count == 0) {
      return NULL;
   }
   struct json_object *tools_array = json_object_new_array();
   /* Held across the walk so enable-flag writers (llm_tools_set_enabled,
    * llm_tools_refresh) don't mutate mid-read. */
   pthread_mutex_lock(&llm_tools_mutex);
   for (int i = 0; i < llm_tools_count; i++) {
      const tool_definition_t *t = &llm_tools_table[i];
      if (llm_tools_enabled_for_session(t, is_remote_session)) {
         json_object_array_add(tools_array, tool_schema(t, claude));
      }
   }
   pthread_mutex_unlock(&llm_tools_mutex);
   if (json_object_array_length(tools_array) == 0) {
      json_object_put(tools_array);
      return NULL;
   }
   return tools_array;
}

struct json_object *llm_tools_get_openai_format_filtered(bool is_remote_session) {
   return format_filtered(is_remote_session, false);
}

/* A conversation's tool set comes from every registered tool but research's
 * own (llm_tools_definitions_hashed). */
static bool frozen_candidate(const tool_definition_t *t) {
   return !research_tool_is_research_only(t->name);
}

/* The last definitions computed, their canonical hashes and the set's
 * fingerprint, and the generations they are of (under llm_tools_mutex):
 * hashed once per generation, not once per turn. */
static char *s_defs;
static char *s_def_hashes;
static char s_defs_fp[DAWN_SHA256_HEX_LEN];
static uint64_t s_defs_registry_gen;
static uint64_t s_defs_table_gen;

/* Caller holds llm_tools_mutex.  The definitions of the registry now. */
static bool defs_rebuild_locked(uint64_t registry_gen, uint64_t table_gen) {
   struct json_object *defs = json_object_new_array();
   for (int i = 0; defs && i < llm_tools_count; i++) {
      if (!frozen_candidate(&llm_tools_table[i])) {
         continue;
      }
      struct json_object *def = neutral_def(&llm_tools_table[i]);
      if (llm_tool_def_valid(def)) {
         json_object_array_add(defs, def);
      } else {
         OLOG_WARNING("tools: '%s' has no definition a request can carry (too large or not "
                      "UTF-8); left out of new conversations",
                      llm_tools_table[i].name);
         json_object_put(def);
      }
   }
   char fp[DAWN_SHA256_HEX_LEN];
   struct json_object *hashes = defs ? llm_tool_defs_hashes(defs, fp) : NULL;
   const int flags = JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE;
   char *json = defs ? strdup(json_object_to_json_string_ext(defs, flags)) : NULL;
   char *hjson = hashes ? strdup(json_object_to_json_string_ext(hashes, flags)) : NULL;
   json_object_put(defs);
   json_object_put(hashes);
   if (!json) {
      free(hjson);
      return false;
   }
   free(s_defs);
   free(s_def_hashes);
   s_defs = json;
   s_def_hashes = hjson; /* NULL: the seam hashes the definitions itself */
   snprintf(s_defs_fp, sizeof(s_defs_fp), "%s", hjson ? fp : "");
   s_defs_registry_gen = registry_gen;
   s_defs_table_gen = table_gen;
   return true;
}

char *llm_tools_definitions_hashed(char **hashes_out, char *fp_out) {
   if (hashes_out) {
      *hashes_out = NULL;
   }
   if (fp_out) {
      fp_out[0] = '\0';
   }
   if (!llm_tools_ready) {
      return NULL;
   }
   pthread_mutex_lock(&llm_tools_mutex);
   /* Read before computing: a change during it rises after, and is seen next. */
   const uint64_t registry_gen = tool_registry_generation();
   const uint64_t table_gen = atomic_load(&llm_tools_generation);
   if (!s_defs || s_defs_registry_gen != registry_gen || s_defs_table_gen != table_gen) {
      (void)defs_rebuild_locked(registry_gen, table_gen);
   }
   char *copy = s_defs ? strdup(s_defs) : NULL;
   /* The three of one generation, or the definitions alone. */
   if (copy && hashes_out && s_def_hashes) {
      *hashes_out = strdup(s_def_hashes);
      if (*hashes_out && fp_out) {
         snprintf(fp_out, DAWN_SHA256_HEX_LEN, "%s", s_defs_fp);
      }
   }
   pthread_mutex_unlock(&llm_tools_mutex);
   return copy;
}

struct json_object *llm_tools_render_frozen(struct json_object *defs, bool claude) {
   if (!json_object_is_type(defs, json_type_array)) {
      return NULL;
   }
   struct json_object *tools_array = json_object_new_array();
   const size_t n = json_object_array_length(defs);
   for (size_t k = 0; tools_array && k < n; k++) {
      struct json_object *def = json_object_array_get_idx(defs, k);
      if (!json_object_is_type(def, json_type_string)) {
         struct json_object *tool = llm_tool_def_render(def, claude);
         if (tool) {
            json_object_array_add(tools_array, tool);
         }
         continue;
      }
      /* A name: an older conversation's set, rendered as the registry has it. */
      const char *name = json_object_get_string(def);
      pthread_mutex_lock(&llm_tools_mutex);
      for (int i = 0; llm_tools_ready && i < llm_tools_count; i++) {
         if (strcmp(llm_tools_table[i].name, name) == 0) {
            json_object_array_add(tools_array, tool_schema(&llm_tools_table[i], claude));
            break;
         }
      }
      pthread_mutex_unlock(&llm_tools_mutex);
   }
   if (tools_array && json_object_array_length(tools_array) == 0) {
      json_object_put(tools_array);
      return NULL;
   }
   return tools_array;
}

struct json_object *llm_tools_request_tools(struct json_object *history,
                                            bool is_remote,
                                            bool claude,
                                            bool inline_ok,
                                            const char **source_out) {
   const char *ignored = NULL;
   const char **source = source_out ? source_out : &ignored;
   session_t *ctx = session_get_command_context();
   if (ctx != NULL && session_tools_suppressed(ctx)) {
      *source = "none: a no-tools turn";
      return NULL;
   }
   if (ctx != NULL && atomic_load(&ctx->research_run_id) > 0) {
      *source = "the research allowlist";
      return format_filtered(is_remote, claude);
   }
   struct json_object *defs = llm_tool_defs_for_request(history, inline_ok);
   if (defs) {
      *source = "the conversation's set";
      struct json_object *tools = llm_tools_render_frozen(defs, claude);
      json_object_put(defs);
      return tools;
   }
   *source = is_remote ? "remote session" : "local session";
   return format_filtered(is_remote, claude);
}

bool llm_tools_request_offers(struct json_object *history, bool is_remote, const char *name) {
   if (!name || !llm_tools_ready) {
      return false;
   }
   session_t *ctx = session_get_command_context();
   const bool research = ctx != NULL && atomic_load(&ctx->research_run_id) > 0;
   if (!research) {
      /* The conversation's tools, when it has them, are what the request
       * defines (in `tools` or in place): a tool they don't name isn't
       * offered. */
      struct json_object *defs = llm_tool_defs_for_request(history, false);
      if (defs) {
         bool named = false;
         const size_t n = json_object_array_length(defs);
         for (size_t k = 0; !named && k < n; k++) {
            const char *f = llm_tool_def_name(json_object_array_get_idx(defs, k));
            named = f && strcmp(f, name) == 0;
         }
         json_object_put(defs);
         if (!named) {
            return false;
         }
      }
   }
   /* Carried, and it must also run: the same check execution makes. */
   bool enabled = false;
   pthread_mutex_lock(&llm_tools_mutex);
   for (int i = 0; i < llm_tools_count; i++) {
      if (strcmp(llm_tools_table[i].name, name) == 0) {
         enabled = llm_tools_enabled_for_session(&llm_tools_table[i], is_remote);
         break;
      }
   }
   pthread_mutex_unlock(&llm_tools_mutex);
   return enabled;
}

/* The last schema hashes computed, and the generations they are of (under
 * llm_tools_mutex): recomputed only when a tool or a schema could have
 * changed, not every turn. */
static char *s_hashes;
static uint64_t s_hashes_registry_gen;
static uint64_t s_hashes_table_gen;

void llm_tools_filter_release(void) {
   pthread_mutex_lock(&llm_tools_mutex);
   free(s_hashes);
   s_hashes = NULL;
   free(s_defs);
   s_defs = NULL;
   free(s_def_hashes);
   s_def_hashes = NULL;
   s_defs_fp[0] = '\0';
   pthread_mutex_unlock(&llm_tools_mutex);
}

char *llm_tools_schema_hashes(void) {
   if (!llm_tools_ready) {
      return NULL;
   }
   pthread_mutex_lock(&llm_tools_mutex);
   /* Read before computing: a change during it rises after, and is seen next. */
   const uint64_t registry_gen = tool_registry_generation();
   const uint64_t table_gen = atomic_load(&llm_tools_generation);
   if (!s_hashes || s_hashes_registry_gen != registry_gen || s_hashes_table_gen != table_gen) {
      struct json_object *out = json_object_new_object();
      for (int i = 0; out && i < llm_tools_count; i++) {
         struct json_object *schema = tool_schema(&llm_tools_table[i], false);
         const char *bytes = json_object_to_json_string_ext(schema, JSON_C_TO_STRING_PLAIN);
         char hash[DAWN_SHA256_HEX_LEN];
         dawn_sha256_hex(bytes, strlen(bytes), hash);
         json_object_object_add(out, llm_tools_table[i].name, json_object_new_string(hash));
         json_object_put(schema);
      }
      char *json = out ? strdup(json_object_to_json_string_ext(out, JSON_C_TO_STRING_PLAIN)) : NULL;
      json_object_put(out);
      if (json) {
         free(s_hashes);
         s_hashes = json;
         s_hashes_registry_gen = registry_gen;
         s_hashes_table_gen = table_gen;
      }
   }
   char *copy = s_hashes ? strdup(s_hashes) : NULL;
   pthread_mutex_unlock(&llm_tools_mutex);
   return copy;
}

/* Append "name" (with ", " separator after the first entry) to a bucket,
 * clamping on truncation so repeated calls can't underflow remaining space.
 * Returns true if the name was fully written, false on truncation or error. */
static bool hint_bucket_append(char *bucket, size_t bucket_size, int *offset, const char *name) {
   if (!bucket || !offset || !name || bucket_size == 0) {
      return false;
   }
   int off = *offset;
   if (off < 0 || (size_t)off >= bucket_size - 1) {
      return false; /* Bucket full */
   }

   if (off > 0) {
      int w = snprintf(bucket + off, bucket_size - off, ", ");
      if (w < 0 || (size_t)w >= bucket_size - off) {
         *offset = (int)bucket_size - 1; /* Clamp */
         return false;
      }
      off += w;
   }

   int w = snprintf(bucket + off, bucket_size - off, "%s", name);
   if (w < 0) {
      return false;
   }
   if ((size_t)w >= bucket_size - off) {
      /* Truncated — clamp so further appends see no remaining space */
      *offset = (int)bucket_size - 1;
      return false;
   }
   off += w;
   *offset = off;
   return true;
}

int llm_tools_build_disabled_hint(bool is_remote, char *buffer, size_t buffer_size) {
   if (!llm_tools_ready || !buffer || buffer_size == 0) {
      return 0;
   }

   /* Bucket tools into two lists for the target session:
    *   unavailable[]      - capability not available (hardware/config not met)
    *   session_disabled[] - capability works, but admin-disabled for this session
    */
   char unavailable[512] = "";
   char session_disabled[512] = "";
   int unavail_off = 0;
   int disabled_off = 0;
   int unavail_count = 0;
   int disabled_count = 0;

   /* Hold llm_tools_mutex across the scan so enable-flag writers
    * (llm_tools_set_enabled, llm_tools_refresh) don't mutate mid-read. */
   pthread_mutex_lock(&llm_tools_mutex);
   for (int i = 0; i < llm_tools_count; i++) {
      const tool_definition_t *t = &llm_tools_table[i];
      bool session_flag = is_remote ? t->enabled_remote : t->enabled_local;

      /* Fully usable in this session - omit from hint */
      if (t->enabled && session_flag) {
         continue;
      }

      if (!t->enabled) {
         /* Capability not available (e.g., HUD hardware offline, API key missing) */
         if (hint_bucket_append(unavailable, sizeof(unavailable), &unavail_off, t->name)) {
            unavail_count++;
         }
      } else {
         /* Capability works but disabled for this session type */
         if (hint_bucket_append(session_disabled, sizeof(session_disabled), &disabled_off,
                                t->name)) {
            disabled_count++;
         }
      }
   }
   pthread_mutex_unlock(&llm_tools_mutex);

   if (unavail_count == 0 && disabled_count == 0) {
      buffer[0] = '\0';
      return 0;
   }

   int len = 0;
   const char *session_label = is_remote ? "remote" : "local";

   if (unavail_count > 0) {
      int w = snprintf(buffer + len, buffer_size - len,
                       "\nNote: The following tools are installed but not currently "
                       "available (hardware offline or not configured): %s. Don't bring this "
                       "up unless the user asks for one of them; then tell them the feature "
                       "exists but isn't reachable right now.\n",
                       unavailable);
      if (w > 0 && (size_t)w < buffer_size - len) {
         len += w;
      } else if (w > 0) {
         len = (int)buffer_size - 1; /* Clamp on truncation */
      }
   }

   if (disabled_count > 0 && (size_t)len < buffer_size - 1) {
      int w = snprintf(buffer + len, buffer_size - len,
                       "\nNote: The following tools are disabled by the administrator "
                       "for this %s session: %s. Don't bring this up unless the user "
                       "asks for one of them; then tell them the feature exists but isn't "
                       "enabled here.\n",
                       session_label, session_disabled);
      if (w > 0 && (size_t)w < buffer_size - len) {
         len += w;
      } else if (w > 0) {
         len = (int)buffer_size - 1;
      }
   }

   return len;
}
