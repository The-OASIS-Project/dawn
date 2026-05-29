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
 * MCP bridge: slot table + pre-generated trampolines + dispatch. Each upstream
 * MCP tool is registered as a native DAWN tool whose callback is a compile-time
 * trampoline that forwards to mcp_bridge_dispatch() with the owning slot. The
 * tool registry shallow-copies metadata, so all pointers handed to it point at
 * slot-owned storage in the static s_slots[] table (stable for process life).
 *
 * Heap-metadata deviation (std-H1): the design proposed heap-allocated metadata
 * for runtime-discovered tools. A fixed static slot array (cap 64) is simpler
 * and equally correct here: the registry copies the struct and the slot keeps
 * the pointed-to strings/params alive. No per-tool heap metadata is needed.
 */

#include <json-c/json.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_mcp.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/mcp_bridge.h"
#include "tools/mcp_bridge_schema.h"
#include "tools/mcp_client.h"
#include "tools/tool_registry.h"

/* One registered bridged tool. `enabled` MUST be first: it doubles as the
 * tool's config struct so dangerous-tool registry validation passes (std-H2). */
typedef struct {
   bool enabled;
   bool in_use;
   bool dangerous;
   char server_alias[TOOL_NAME_MAX];
   char upstream_tool_name[TOOL_NAME_MAX];
   char dawn_tool_name[TOOL_NAME_MAX];
   mcp_client_t *client;
   char *description;      /* wrapped; owned */
   mcp_param_set_t params; /* owned (moved in at registration) */
} mcp_slot_t;

static mcp_slot_t s_slots[MCP_BRIDGE_MAX_SLOTS];
static int s_slot_count;
static pthread_mutex_t s_slots_mutex = PTHREAD_MUTEX_INITIALIZER;

static char *mcp_bridge_dispatch(mcp_slot_t *slot,
                                 const char *action,
                                 char *value,
                                 int *should_respond);

/* Dangerous tools must carry a config + parser (validate_dangerous_tool); the
 * slot itself is the config (enabled-first) and this parser is a no-op because
 * bridged tools have no per-tool TOML section. */
static void mcp_slot_noop_parser(toml_table_t *table, void *config) {
   (void)table;
   (void)config;
}

/* --------------------------------------------------------------------------
 * Pre-generated trampolines (one per slot; cap = MCP_BRIDGE_MAX_SLOTS = 64)
 * -------------------------------------------------------------------------- */

#define MCP_TRAMPOLINE(N)                                    \
   static char *mcp_cb_##N(const char *a, char *v, int *r) { \
      return mcp_bridge_dispatch(&s_slots[N], a, v, r);      \
   }

/* clang-format off */
MCP_TRAMPOLINE(0)  MCP_TRAMPOLINE(1)  MCP_TRAMPOLINE(2)  MCP_TRAMPOLINE(3)
MCP_TRAMPOLINE(4)  MCP_TRAMPOLINE(5)  MCP_TRAMPOLINE(6)  MCP_TRAMPOLINE(7)
MCP_TRAMPOLINE(8)  MCP_TRAMPOLINE(9)  MCP_TRAMPOLINE(10) MCP_TRAMPOLINE(11)
MCP_TRAMPOLINE(12) MCP_TRAMPOLINE(13) MCP_TRAMPOLINE(14) MCP_TRAMPOLINE(15)
MCP_TRAMPOLINE(16) MCP_TRAMPOLINE(17) MCP_TRAMPOLINE(18) MCP_TRAMPOLINE(19)
MCP_TRAMPOLINE(20) MCP_TRAMPOLINE(21) MCP_TRAMPOLINE(22) MCP_TRAMPOLINE(23)
MCP_TRAMPOLINE(24) MCP_TRAMPOLINE(25) MCP_TRAMPOLINE(26) MCP_TRAMPOLINE(27)
MCP_TRAMPOLINE(28) MCP_TRAMPOLINE(29) MCP_TRAMPOLINE(30) MCP_TRAMPOLINE(31)
MCP_TRAMPOLINE(32) MCP_TRAMPOLINE(33) MCP_TRAMPOLINE(34) MCP_TRAMPOLINE(35)
MCP_TRAMPOLINE(36) MCP_TRAMPOLINE(37) MCP_TRAMPOLINE(38) MCP_TRAMPOLINE(39)
MCP_TRAMPOLINE(40) MCP_TRAMPOLINE(41) MCP_TRAMPOLINE(42) MCP_TRAMPOLINE(43)
MCP_TRAMPOLINE(44) MCP_TRAMPOLINE(45) MCP_TRAMPOLINE(46) MCP_TRAMPOLINE(47)
MCP_TRAMPOLINE(48) MCP_TRAMPOLINE(49) MCP_TRAMPOLINE(50) MCP_TRAMPOLINE(51)
MCP_TRAMPOLINE(52) MCP_TRAMPOLINE(53) MCP_TRAMPOLINE(54) MCP_TRAMPOLINE(55)
MCP_TRAMPOLINE(56) MCP_TRAMPOLINE(57) MCP_TRAMPOLINE(58) MCP_TRAMPOLINE(59)
MCP_TRAMPOLINE(60) MCP_TRAMPOLINE(61) MCP_TRAMPOLINE(62) MCP_TRAMPOLINE(63)
    /* clang-format on */

    static const tool_callback_fn s_trampolines[MCP_BRIDGE_MAX_SLOTS] = {
       mcp_cb_0,  mcp_cb_1,  mcp_cb_2,  mcp_cb_3,  mcp_cb_4,  mcp_cb_5,  mcp_cb_6,  mcp_cb_7,
       mcp_cb_8,  mcp_cb_9,  mcp_cb_10, mcp_cb_11, mcp_cb_12, mcp_cb_13, mcp_cb_14, mcp_cb_15,
       mcp_cb_16, mcp_cb_17, mcp_cb_18, mcp_cb_19, mcp_cb_20, mcp_cb_21, mcp_cb_22, mcp_cb_23,
       mcp_cb_24, mcp_cb_25, mcp_cb_26, mcp_cb_27, mcp_cb_28, mcp_cb_29, mcp_cb_30, mcp_cb_31,
       mcp_cb_32, mcp_cb_33, mcp_cb_34, mcp_cb_35, mcp_cb_36, mcp_cb_37, mcp_cb_38, mcp_cb_39,
       mcp_cb_40, mcp_cb_41, mcp_cb_42, mcp_cb_43, mcp_cb_44, mcp_cb_45, mcp_cb_46, mcp_cb_47,
       mcp_cb_48, mcp_cb_49, mcp_cb_50, mcp_cb_51, mcp_cb_52, mcp_cb_53, mcp_cb_54, mcp_cb_55,
       mcp_cb_56, mcp_cb_57, mcp_cb_58, mcp_cb_59, mcp_cb_60, mcp_cb_61, mcp_cb_62, mcp_cb_63,
    };

/* --------------------------------------------------------------------------
 * Dispatch
 * -------------------------------------------------------------------------- */

static char *dispatch_error(int *should_respond, const char *msg) {
   if (should_respond != NULL) {
      *should_respond = 1;
   }
   return strdup(msg);
}

/*
 * Forward one LLM tool call to the upstream MCP server via tools/call.
 *
 * ARG DELIVERY (deferred seam): `value` is treated as the JSON arguments object.
 * The executor's (action, value) packing does not yet deliver clean typed JSON
 * for arbitrary MCP params; the executor-integration step (Step 7+) wires a
 * raw-args hook so `value` carries the original arguments JSON. Until then this
 * dispatch is exercised by the unit test passing JSON directly in `value`.
 */
static char *mcp_bridge_dispatch(mcp_slot_t *slot,
                                 const char *action,
                                 char *value,
                                 int *should_respond) {
   (void)action; /* MCP tools/call has no separate action; all args are in value */

   if (slot == NULL || !slot->in_use || slot->client == NULL) {
      return dispatch_error(should_respond, "MCP tool is not available.");
   }

   /* Per-call MCP access re-check (sec-H4): visibility can change mid-session. */
   int64_t uid = tool_get_current_user_id();
   bool allowed = false;
   if (auth_db_mcp_check_access(uid, slot->server_alias, &allowed) != AUTH_DB_SUCCESS || !allowed) {
      OLOG_WARNING("MCP bridge: user %lld denied access to server '%s'", (long long)uid,
                   slot->server_alias);
      return dispatch_error(should_respond, "Access to this MCP server is not enabled for you.");
   }

   /* cbm dangerous-tool denylist (sec-M1): admin-only invocation. */
   if (slot->dangerous) {
      bool is_admin = false;
      if (auth_db_mcp_user_is_admin(uid, &is_admin) != AUTH_DB_SUCCESS || !is_admin) {
         OLOG_WARNING("MCP bridge: non-admin user %lld blocked from dangerous tool '%s'",
                      (long long)uid, slot->dawn_tool_name);
         return dispatch_error(should_respond, "This MCP tool is restricted to administrators.");
      }
   }

   /* HOOK (Step 14): auto-fill `project` from the session's active project when
    * the LLM omits it for a code-graph tool. Deferred until the code-projects
    * layer exists. */

   /* Build tools/call params: { "name": <upstream>, "arguments": <args> }. */
   struct json_object *p = json_object_new_object();
   json_object_object_add(p, "name", json_object_new_string(slot->upstream_tool_name));
   struct json_object *args = (value != NULL && value[0] != '\0') ? json_tokener_parse(value)
                                                                  : NULL;
   if (args == NULL || !json_object_is_type(args, json_type_object)) {
      if (args != NULL) {
         json_object_put(args);
      }
      args = json_object_new_object();
   }
   json_object_object_add(p, "arguments", args);

   char *result = NULL;
   int rc = mcp_client_call(slot->client, "tools/call", json_object_to_json_string(p), 0, NULL,
                            NULL, &result);
   json_object_put(p);

   if (should_respond != NULL) {
      *should_respond = 1;
   }
   if (rc == SUCCESS && result != NULL) {
      return result;
   }
   free(result);
   OLOG_ERROR("MCP bridge: tools/call '%s' on '%s' failed (rc=%d)", slot->upstream_tool_name,
              slot->server_alias, rc);
   return strdup("MCP tool call failed.");
}

/* --------------------------------------------------------------------------
 * Registration
 * -------------------------------------------------------------------------- */

int mcp_bridge_register_tool(mcp_client_t *client,
                             const char *server_alias,
                             const char *upstream_tool_name,
                             const char *dawn_tool_name,
                             const char *description,
                             mcp_param_set_t *params,
                             bool dangerous) {
   if (client == NULL || server_alias == NULL || upstream_tool_name == NULL ||
       dawn_tool_name == NULL || params == NULL) {
      return FAILURE;
   }

   pthread_mutex_lock(&s_slots_mutex);
   if (s_slot_count >= MCP_BRIDGE_MAX_SLOTS) {
      pthread_mutex_unlock(&s_slots_mutex);
      OLOG_ERROR("MCP bridge: slot table full (%d)", MCP_BRIDGE_MAX_SLOTS);
      return FAILURE;
   }
   int idx = s_slot_count;
   mcp_slot_t *slot = &s_slots[idx];
   memset(slot, 0, sizeof(*slot));
   slot->enabled = true;
   slot->dangerous = dangerous;
   slot->client = client;
   snprintf(slot->server_alias, sizeof(slot->server_alias), "%s", server_alias);
   snprintf(slot->upstream_tool_name, sizeof(slot->upstream_tool_name), "%s", upstream_tool_name);
   snprintf(slot->dawn_tool_name, sizeof(slot->dawn_tool_name), "%s", dawn_tool_name);
   slot->description = mcp_schema_wrap_description(server_alias, description);
   slot->params = *params; /* move ownership */

   tool_metadata_t meta;
   memset(&meta, 0, sizeof(meta));
   meta.name = slot->dawn_tool_name;
   meta.device_string = slot->dawn_tool_name;
   meta.description = slot->description;
   meta.params = slot->params.params;
   meta.param_count = slot->params.param_count;
   meta.device_type = TOOL_DEVICE_TYPE_GETTER;
   meta.capabilities = TOOL_CAP_NETWORK | (dangerous ? TOOL_CAP_DANGEROUS : TOOL_CAP_NONE);
   meta.default_local = true;
   meta.default_remote = true;
   meta.callback = s_trampolines[idx];
   /* enabled-first slot serves as the config struct so dangerous-tool
    * validation (needs config + parser) passes; no TOML section. */
   meta.config = slot;
   meta.config_size = sizeof(*slot);
   meta.config_parser = mcp_slot_noop_parser;

   if (tool_registry_register(&meta) != 0) {
      OLOG_ERROR("MCP bridge: failed to register tool '%s'", dawn_tool_name);
      free(slot->description);
      slot->description = NULL;
      mcp_param_set_free(&slot->params);
      memset(slot, 0, sizeof(*slot));
      pthread_mutex_unlock(&s_slots_mutex);
      return FAILURE;
   }

   slot->in_use = true;
   s_slot_count++;
   pthread_mutex_unlock(&s_slots_mutex);

   /* Caller's set was moved into the slot; clear it so their free is a no-op. */
   params->params = NULL;
   params->param_count = 0;

   OLOG_INFO("MCP bridge: registered tool '%s' (server '%s'%s)", dawn_tool_name, server_alias,
             dangerous ? ", dangerous" : "");
   return SUCCESS;
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

int mcp_bridge_init(void) {
   /* Config-agnostic core: nothing to wire until config-driven server setup
    * lands (Step 7). Servers + their tools are registered there. */
   return SUCCESS;
}

void mcp_bridge_shutdown(void) {
   pthread_mutex_lock(&s_slots_mutex);
   for (int i = 0; i < s_slot_count; i++) {
      mcp_slot_t *slot = &s_slots[i];
      if (!slot->in_use) {
         continue;
      }
      free(slot->description);
      slot->description = NULL;
      mcp_param_set_free(&slot->params);
      slot->in_use = false;
   }
   s_slot_count = 0;
   pthread_mutex_unlock(&s_slots_mutex);
}
