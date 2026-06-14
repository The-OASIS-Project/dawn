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
#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "tools/mcp_bridge.h"
#include "tools/mcp_bridge_schema.h"
#include "tools/mcp_client.h"
#include "tools/mcp_transport_http_sse.h"
#include "tools/tool_registry.h"

#ifdef DAWN_ENABLE_CODE_PROJECTS
#include "tools/code_project_db.h"
#include "tools/code_project_namemap.h"

/* Translate one clean identifier arg (LLM namespace) to cbm's path-derived graph
 * namespace in place. Only `project` and `qualified_name` carry the slug; bare
 * symbol names (function_name) and search patterns are left untouched. */
static void namemap_translate_arg(struct json_object *args, const char *key) {
   struct json_object *v = NULL;
   if (!json_object_object_get_ex(args, key, &v) || !json_object_is_type(v, json_type_string)) {
      return;
   }
   const char *clean = json_object_get_string(v);
   char graph[CONFIG_PATH_MAX];
   code_project_namemap_to_graph(clean, graph, sizeof(graph));
   if (graph[0] != '\0' && strcmp(graph, clean) != 0) {
      json_object_object_add(args, key, json_object_new_string(graph)); /* replaces existing key */
   }
}
#endif

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

/* One connected upstream server; owns the client for the bridge's lifetime. */
typedef struct {
   char alias[MCP_SERVER_ALIAS_MAX];
   mcp_client_t *client;
   bool in_use;
} mcp_server_entry_t;

static mcp_server_entry_t s_servers[MCP_SERVERS_MAX];
static int s_server_count;

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
 * ARG DELIVERY: prefer the executor's raw-args hook (the original typed JSON the
 * LLM sent), which the (action, value) packing flattens lossily. Fall back to
 * `value` when there is no executor context (e.g. the unit test invokes the
 * callback directly with JSON in `value`).
 */
static char *mcp_bridge_dispatch(mcp_slot_t *slot,
                                 const char *action,
                                 char *value,
                                 int *should_respond) {
   (void)action; /* MCP tools/call has no separate action; all args are in the JSON object */

   if (slot == NULL || !slot->in_use || slot->client == NULL) {
      return dispatch_error(should_respond, "MCP tool is not available.");
   }

   /* Fail closed: bridged MCP tools require an authenticated user session. The
    * registry's tool_get_current_user_id() invents uid 1 (admin) when no session
    * context is set, which would let any non-session caller bypass both gates
    * below (sec-S1). */
   session_t *cmd_sess = session_get_command_context();
   if (cmd_sess == NULL || cmd_sess->metrics.user_id <= 0) {
      return dispatch_error(should_respond, "MCP tools require an authenticated user session.");
   }
   int64_t uid = cmd_sess->metrics.user_id;

   /* Per-call MCP access re-check (sec-H4): visibility can change mid-session. */
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

   /* Build tools/call params: { "name": <upstream>, "arguments": <args> }. */
   const char *args_json = llm_tools_current_raw_args();
   if (args_json == NULL) {
      args_json = value; /* no executor context (unit test path) */
   }
   struct json_object *p = json_object_new_object();
   json_object_object_add(p, "name", json_object_new_string(slot->upstream_tool_name));
   struct json_object *args = (args_json != NULL && args_json[0] != '\0')
                                  ? json_tokener_parse(args_json)
                                  : NULL;
   if (args == NULL || !json_object_is_type(args, json_type_object)) {
      if (args != NULL) {
         json_object_put(args);
      }
      args = json_object_new_object();
   }

#ifdef DAWN_ENABLE_CODE_PROJECTS
   /* Auto-fill `project` from the session's active project for cbm tools when the
    * LLM omitted it, after re-checking the user can still see that project
    * (sec-H4 per-call visibility). Then translate the LLM's clean identifiers
    * into cbm's path-derived namespace so the on-disk slug/paths never have to be
    * known (or seen) by the LLM (the result is scrubbed symmetrically below). */
   if (strcmp(slot->server_alias, "cbm") == 0) {
      struct json_object *existing = NULL;
      if (!json_object_object_get_ex(args, "project", &existing)) {
         char active[64] = { 0 };
         pthread_mutex_lock(&cmd_sess->llm_config_mutex);
         snprintf(active, sizeof(active), "%s", cmd_sess->active_project_name);
         pthread_mutex_unlock(&cmd_sess->llm_config_mutex);
         bool visible = false;
         if (active[0] != '\0' &&
             code_project_db_check_visible(uid, active, &visible) == AUTH_DB_SUCCESS && visible) {
            json_object_object_add(args, "project", json_object_new_string(active));
         }
      }
      namemap_translate_arg(args, "project");
      namemap_translate_arg(args, "qualified_name");
   }
#endif

   json_object_object_add(p, "arguments", args);

   char *result = NULL;
   int rc = mcp_client_call(slot->client, "tools/call", json_object_to_json_string(p), 0, NULL,
                            NULL, &result);
   json_object_put(p);

   if (should_respond != NULL) {
      *should_respond = 1;
   }
   if (rc == SUCCESS && result != NULL) {
#ifdef DAWN_ENABLE_CODE_PROJECTS
      /* Strip cbm's graph-name prefix and absolute source_root paths out of the
       * result so no slug or filesystem layout reaches the LLM (symmetric with
       * the outbound translation above). */
      if (strcmp(slot->server_alias, "cbm") == 0) {
         char *scrubbed = code_project_namemap_scrub(result);
         if (scrubbed != NULL) {
            free(result);
            result = scrubbed;
         }
      }
#endif
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

   if (tool_registry_register(&meta) != SUCCESS) {
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

/* cbm mutating-tool denylist (sec-M1): registered TOOL_CAP_DANGEROUS + admin-only
 * invocation. Matched on the upstream tool name. */
static bool is_dangerous_tool(const char *upstream_name) {
   static const char *const denylist[] = { "index_repository", "delete_project", "manage_adr",
                                           NULL };
   for (int i = 0; denylist[i] != NULL; i++) {
      if (strcmp(upstream_name, denylist[i]) == 0) {
         return true;
      }
   }
   return false;
}

/* Register every tool from a connected server's cached tools/list result. */
static void register_server_tools(const char *alias, mcp_client_t *client) {
   const char *tools_json = mcp_client_tools_json(client);
   if (tools_json == NULL) {
      OLOG_WARNING("MCP bridge: server '%s' returned no tools", alias);
      return;
   }
   struct json_object *root = json_tokener_parse(tools_json);
   struct json_object *tools = NULL;
   if (root == NULL || !json_object_object_get_ex(root, "tools", &tools) ||
       !json_object_is_type(tools, json_type_array)) {
      OLOG_WARNING("MCP bridge: server '%s' tools/list payload not understood", alias);
      if (root != NULL) {
         json_object_put(root);
      }
      return;
   }

   int registered = 0;
   int n = json_object_array_length(tools);
   for (int i = 0; i < n; i++) {
      struct json_object *tool = json_object_array_get_idx(tools, i);
      struct json_object *name_obj = NULL;
      if (tool == NULL || !json_object_object_get_ex(tool, "name", &name_obj)) {
         continue;
      }
      const char *upstream = json_object_get_string(name_obj);
      if (upstream == NULL || upstream[0] == '\0') {
         continue;
      }
      struct json_object *desc_obj = NULL;
      const char *description = json_object_object_get_ex(tool, "description", &desc_obj)
                                    ? json_object_get_string(desc_obj)
                                    : "";
      struct json_object *schema = NULL;
      json_object_object_get_ex(tool, "inputSchema", &schema);

      mcp_param_set_t params = { 0 };
      if (mcp_schema_translate(schema, upstream, &params) != SUCCESS) {
         OLOG_WARNING("MCP bridge: skipping tool '%s/%s' (schema rejected)", alias, upstream);
         continue;
      }

      char dawn_name[TOOL_NAME_MAX];
      snprintf(dawn_name, sizeof(dawn_name), "%s_%s", alias, upstream);

      if (mcp_bridge_register_tool(client, alias, upstream, dawn_name, description, &params,
                                   is_dangerous_tool(upstream)) == SUCCESS) {
         registered++;
      } else {
         mcp_param_set_free(&params); /* register failed: reclaim (move didn't happen) */
      }
   }
   json_object_put(root);
   OLOG_INFO("MCP bridge: registered %d tool(s) from server '%s'", registered, alias);
}

int mcp_bridge_init(void) {
   const dawn_config_t *cfg = config_get();
   if (cfg == NULL || !cfg->mcp.enabled) {
      return SUCCESS; /* bridge disabled */
   }

   for (int i = 0; i < cfg->mcp.server_count && i < MCP_SERVERS_MAX; i++) {
      const mcp_server_config_t *srv = &cfg->mcp.servers[i];
      if (!srv->enabled) {
         continue;
      }
      if (s_server_count >= MCP_SERVERS_MAX) {
         break;
      }

      /* Bearer token from the env var named in config (Phase 1 source). */
      const char *token = (srv->auth_bearer_env[0] != '\0') ? getenv(srv->auth_bearer_env) : NULL;

      mcp_client_opts_t opts = {
         .name = srv->alias,
         .transport_factory = mcp_transport_http_sse_create,
         .url = srv->url,
         .bearer_token = token,
         .tls_verify = srv->tls_verify,
         .connect_timeout_seconds = srv->request_timeout_seconds,
         .idle_close_seconds = srv->idle_close_seconds,
         .request_timeout_ms = (srv->request_timeout_seconds > 0 ? srv->request_timeout_seconds
                                                                 : 30) *
                               1000L,
         .client_name = "DAWN",
         .client_version = "1.0",
      };
      mcp_client_t *client = mcp_client_create(&opts);
      if (client == NULL) {
         OLOG_WARNING("MCP bridge: failed to create client for server '%s'", srv->alias);
         continue;
      }
      if (mcp_client_connect(client) != SUCCESS) {
         OLOG_WARNING("MCP bridge: could not connect to server '%s' (%s); skipping", srv->alias,
                      srv->url);
         mcp_client_destroy(client);
         continue;
      }

      snprintf(s_servers[s_server_count].alias, sizeof(s_servers[s_server_count].alias), "%s",
               srv->alias);
      s_servers[s_server_count].client = client;
      s_servers[s_server_count].in_use = true;
      s_server_count++;

      register_server_tools(srv->alias, client);
      /* Admin access bootstrap (auth_db_mcp_grant_all_admins) intentionally does
       * NOT run here: mcp_bridge_init() executes during tools_register_all(),
       * before auth_db_init(), so the grant would hit a closed DB. dawn.c runs it
       * after auth_db_init() for every configured server instead. */
   }

   OLOG_INFO("MCP bridge: initialized with %d connected server(s)", s_server_count);

#ifdef DAWN_ENABLE_CODE_PROJECTS
   /* Capture cbm's path-derived graph-name prefix now that the client is
    * connected, so the name-translation boundary works on the first cbm tool
    * call after a restart (projects already indexed). Refreshed post-index. */
   code_project_namemap_capture();
#endif

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

   /* Shut down + free the clients the bridge owns (registered tools that still
    * reference them are torn down with the registry at process exit). Held under
    * the slots mutex so a concurrent call_tool/reconnect can't read a freed
    * client pointer from the table (sec-S4). */
   pthread_mutex_lock(&s_slots_mutex);
   for (int i = 0; i < s_server_count; i++) {
      if (s_servers[i].in_use && s_servers[i].client != NULL) {
         mcp_client_destroy(s_servers[i].client);
         s_servers[i].client = NULL;
         s_servers[i].in_use = false;
      }
   }
   s_server_count = 0;
   pthread_mutex_unlock(&s_slots_mutex);
}

int mcp_bridge_status_text(char *out, size_t out_len, int *bytes_written_out) {
   if (out == NULL || out_len == 0) {
      return FAILURE;
   }
   int off = 0;
   pthread_mutex_lock(&s_slots_mutex);
   if (s_server_count == 0) {
      off = snprintf(out, out_len, "MCP bridge: no servers connected.\n");
   } else {
      for (int i = 0; i < s_server_count && off < (int)out_len; i++) {
         int tools = 0;
         for (int j = 0; j < s_slot_count; j++) {
            if (s_slots[j].in_use && strcmp(s_slots[j].server_alias, s_servers[i].alias) == 0) {
               tools++;
            }
         }
         const char *state;
         switch (mcp_client_state(s_servers[i].client)) {
            case MCP_STATE_CONNECTED:
               state = "connected";
               break;
            case MCP_STATE_DISABLED:
               state = "disabled";
               break;
            case MCP_STATE_DISCONNECTED:
               state = "disconnected";
               break;
            default:
               state = "connecting";
               break;
         }
         off += snprintf(out + off, out_len - off, "%s: %s, %d tool(s)\n", s_servers[i].alias,
                         state, tools);
      }
   }
   pthread_mutex_unlock(&s_slots_mutex);
   if (bytes_written_out != NULL) {
      *bytes_written_out = off;
   }
   return SUCCESS;
}

int mcp_bridge_reconnect(int *connected_out) {
   /* Snapshot the client pointers under the lock so a concurrent shutdown or
    * call_tool can't tear the table out from under us (sec-S4), then run the
    * blocking reset/connect without holding the lock. */
   mcp_client_t *clients[MCP_SERVERS_MAX];
   int n = 0;
   pthread_mutex_lock(&s_slots_mutex);
   for (int i = 0; i < s_server_count; i++) {
      if (s_servers[i].in_use && s_servers[i].client != NULL) {
         clients[n++] = s_servers[i].client;
      }
   }
   pthread_mutex_unlock(&s_slots_mutex);

   int connected = 0;
   for (int i = 0; i < n; i++) {
      mcp_client_reset(clients[i]);
      if (mcp_client_connect(clients[i]) == SUCCESS) {
         connected++;
      }
   }
   if (connected_out != NULL) {
      *connected_out = connected;
   }
   return SUCCESS;
}

int mcp_bridge_server_connected(const char *server_alias) {
   if (server_alias == NULL) {
      return FAILURE;
   }
   int connected = FAILURE;
   pthread_mutex_lock(&s_slots_mutex);
   for (int i = 0; i < s_server_count; i++) {
      if (s_servers[i].in_use && strcmp(s_servers[i].alias, server_alias) == 0) {
         if (mcp_client_state(s_servers[i].client) == MCP_STATE_CONNECTED) {
            connected = SUCCESS;
         }
         break;
      }
   }
   pthread_mutex_unlock(&s_slots_mutex);
   return connected;
}

int mcp_bridge_call_tool(const char *server_alias,
                         const char *tool_name,
                         const char *args_json,
                         long timeout_ms,
                         char **result_out) {
   if (result_out != NULL) {
      *result_out = NULL;
   }
   if (server_alias == NULL || tool_name == NULL) {
      return FAILURE;
   }

   mcp_client_t *client = NULL;
   pthread_mutex_lock(&s_slots_mutex);
   for (int i = 0; i < s_server_count; i++) {
      if (s_servers[i].in_use && strcmp(s_servers[i].alias, server_alias) == 0) {
         client = s_servers[i].client;
         break;
      }
   }
   pthread_mutex_unlock(&s_slots_mutex);
   if (client == NULL) {
      OLOG_WARNING("MCP bridge: no connected server '%s' for tool '%s'", server_alias, tool_name);
      return FAILURE;
   }

   struct json_object *p = json_object_new_object();
   json_object_object_add(p, "name", json_object_new_string(tool_name));
   struct json_object *args = (args_json != NULL && args_json[0] != '\0')
                                  ? json_tokener_parse(args_json)
                                  : NULL;
   if (args == NULL || !json_object_is_type(args, json_type_object)) {
      if (args != NULL) {
         json_object_put(args);
      }
      args = json_object_new_object();
   }
   json_object_object_add(p, "arguments", args);

   int rc = mcp_client_call(client, "tools/call", json_object_to_json_string(p), timeout_ms, NULL,
                            NULL, result_out);
   json_object_put(p);
   return rc == SUCCESS ? SUCCESS : FAILURE;
}
