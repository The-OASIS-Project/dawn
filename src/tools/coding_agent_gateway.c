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
 * DAWN's client for the agent gateway: one call path (the user it acts for
 * added by DAWN, never taken from a model), the job <-> client_ref names, and
 * cancels that run off the caller's thread.
 */

#include "tools/coding_agent_gateway.h"

#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config/dawn_config.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/mcp_bridge.h"
#include "utils/string_utils.h"

#define GW_REASON_MAX 200    /* the gateway's reason kept in an error */
#define GW_MESSAGE_MAX 320   /* DAWN's words around it */
#define GW_STOP_WAIT_MS 3000 /* each of stop's two waits for async calls */
#define GW_STOP_POLL_MS 50

static atomic_bool s_stopping;
static atomic_int s_in_flight;

const char *coding_gateway_alias(void) {
   if (!g_config.mcp.enabled) {
      return NULL;
   }
   for (int i = 0; i < g_config.mcp.server_count && i < MCP_SERVERS_MAX; i++) {
      const mcp_server_config_t *srv = &g_config.mcp.servers[i];
      if (srv->enabled && strcmp(srv->role, MCP_SERVER_ROLE_AGENT_GATEWAY) == 0) {
         return srv->alias;
      }
   }
   return NULL;
}

/* @p lead, then the gateway's @p reason as one line cut on a whole character. */
static char *message(const char *lead, const char *reason) {
   char line[GW_REASON_MAX + 64];
   str_excerpt_line(reason ? reason : "", GW_REASON_MAX, line, sizeof(line));
   char *out = malloc(GW_MESSAGE_MAX);
   if (out != NULL) {
      snprintf(out, GW_MESSAGE_MAX, "%s%s%s", lead, line[0] ? ": " : ".", line);
   }
   return out;
}

int coding_gateway_call(int user_id,
                        const char *tool,
                        struct json_object *args,
                        struct json_object **out,
                        char **err) {
   *out = NULL;
   *err = NULL;
   const char *alias = coding_gateway_alias();
   if (alias == NULL) {
      json_object_put(args);
      *err = strdup("No coding-agent gateway is set up.");
      return CODING_GW_FAILED;
   }
   if (mcp_bridge_ensure_connected(alias) != SUCCESS) {
      json_object_put(args);
      *err = strdup("The coding-agent gateway isn't reachable right now.");
      return CODING_GW_FAILED;
   }
   if (!mcp_bridge_is_agent_gateway(alias)) {
      json_object_put(args);
      *err = strdup("The server set up as the coding-agent gateway doesn't offer its tools.");
      return CODING_GW_FAILED;
   }
   if (user_id > 0) {
      json_object_object_add(args, "dawn_user_id", json_object_new_int(user_id));
   }
   char *text = NULL;
   const int rc = mcp_bridge_call_tool(alias, tool, json_object_to_json_string(args), 0, &text);
   json_object_put(args);
   if (rc != SUCCESS) {
      /* Text comes back only when the gateway itself said no. */
      const int why = text ? CODING_GW_REFUSED : CODING_GW_FAILED;
      *err = message(text ? "The coding-agent gateway refused"
                          : "The coding-agent gateway didn't answer",
                     text);
      free(text);
      return why;
   }
   *out = text ? json_tokener_parse(text) : NULL;
   free(text);
   if (*out == NULL || !json_object_is_type(*out, json_type_object)) {
      json_object_put(*out);
      *out = NULL;
      *err = strdup("The coding-agent gateway sent an answer DAWN can't read.");
      return CODING_GW_FAILED;
   }
   return CODING_GW_OK;
}

const char *coding_gateway_jstr(struct json_object *o, const char *key) {
   struct json_object *v = NULL;
   if (o == NULL || !json_object_object_get_ex(o, key, &v) || v == NULL ||
       json_object_is_type(v, json_type_null)) {
      return "";
   }
   return json_object_get_string(v);
}

void coding_gateway_ref(int64_t conv_id, char *out, size_t out_len) {
   snprintf(out, out_len, CODING_AGENT_CLIENT_REF_FMT, (long long)conv_id);
}

#define CG_HOST_MAX 253
#define CG_LABEL_MAX 63
#define CG_TOOL_NAME_MAX 64

/* A host name DAWN will show and allow: lowercase LDH labels, dot-separated. */
static bool host_ok(const char *h) {
   const size_t n = strlen(h);
   if (n == 0 || n > CG_HOST_MAX || strchr(h, '.') == NULL) {
      return false;
   }
   size_t label = 0;
   for (size_t i = 0; i <= n; i++) {
      const char c = h[i];
      if (c == '.' || c == '\0') {
         if (label == 0 || label > CG_LABEL_MAX || h[i - 1] == '-') {
            return false; /* empty, too long, or ending in '-' */
         }
         label = 0;
      } else if (islower((unsigned char)c) || isdigit((unsigned char)c) ||
                 (c == '-' && label > 0)) {
         label++;
      } else {
         return false;
      }
   }
   return true;
}

static bool tool_name_ok(const char *t) {
   const size_t n = strlen(t);
   if (n == 0 || n > CG_TOOL_NAME_MAX) {
      return false;
   }
   for (size_t i = 0; i < n; i++) {
      const unsigned char c = (unsigned char)t[i];
      if (!isalnum(c) && c != '_' && c != '.' && c != ':' && c != '-') {
         return false;
      }
   }
   return true;
}

bool coding_gateway_request_line(struct json_object *req, char *out, size_t out_len) {
   const char *kind = coding_gateway_jstr(req, "kind");
   const char *value = coding_gateway_jstr(json_object_object_get(req, "payload"), "value");
   /* The value is the agent's: quoted as a name, never read as words. */
   if (strcmp(kind, "domain") == 0 && host_ok(value)) {
      const bool encoded = strncmp(value, "xn--", 4) == 0 || strstr(value, ".xn--") != NULL;
      snprintf(out, out_len, "reach the host `%s`%s", value,
               encoded ? " (an international name, shown encoded)" : "");
   } else if (strcmp(kind, "tool") == 0 && tool_name_ok(value)) {
      snprintf(out, out_len, "use the tool `%s`", value);
   } else if (strcmp(kind, "question") == 0) {
      snprintf(out, out_len, "ask you a question");
   } else {
      return false;
   }
   return true;
}

int64_t coding_gateway_ref_job(const char *client_ref) {
   long long id = 0;
   char tail;
   return client_ref && sscanf(client_ref, CODING_AGENT_CLIENT_REF_FMT "%c", &id, &tail) == 1 &&
                  id > 0
              ? (int64_t)id
              : 0;
}

void coding_gateway_cancel(int user_id, int64_t conv_id) {
   char ref[CODING_AGENT_REF_MAX];
   coding_gateway_ref(conv_id, ref, sizeof(ref));
   struct json_object *args = json_object_new_object();
   json_object_object_add(args, "client_ref", json_object_new_string(ref));
   struct json_object *ans = NULL;
   char *err = NULL;
   if (coding_gateway_call(user_id, "task_cancel", args, &ans, &err) != CODING_GW_OK) {
      OLOG_WARNING("coding_agent: cancel of job %lld: %s", (long long)conv_id,
                   err ? err : "failed");
   }
   json_object_put(ans);
   free(err);
}

typedef struct {
   int64_t conv_id;
   int user_id;
} gw_cancel_t;

static void *cancel_thread(void *arg) {
   gw_cancel_t c = *(gw_cancel_t *)arg;
   free(arg);
   coding_gateway_cancel(c.user_id, c.conv_id);
   atomic_fetch_sub(&s_in_flight, 1);
   return NULL;
}

void coding_gateway_cancel_async(int user_id, int64_t conv_id) {
   /* Counted before the check, so stop() either waits for this one or this
    * one sees it stopping. */
   atomic_fetch_add(&s_in_flight, 1);
   if (atomic_load(&s_stopping)) {
      atomic_fetch_sub(&s_in_flight, 1);
      OLOG_WARNING("coding_agent: shutting down; job %lld's task wasn't asked to stop",
                   (long long)conv_id);
      return;
   }
   gw_cancel_t *c = malloc(sizeof(*c));
   pthread_attr_t attr;
   if (c == NULL || pthread_attr_init(&attr) != 0) {
      free(c);
      atomic_fetch_sub(&s_in_flight, 1);
      OLOG_WARNING("coding_agent: couldn't ask the gateway to stop job %lld", (long long)conv_id);
      return;
   }
   c->conv_id = conv_id;
   c->user_id = user_id;
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   pthread_t th;
   if (pthread_create(&th, &attr, cancel_thread, c) != 0) {
      atomic_fetch_sub(&s_in_flight, 1);
      free(c);
      OLOG_WARNING("coding_agent: couldn't ask the gateway to stop job %lld", (long long)conv_id);
   }
   pthread_attr_destroy(&attr);
}

static void wait_in_flight(void) {
   for (int waited = 0; atomic_load(&s_in_flight) > 0 && waited < GW_STOP_WAIT_MS;
        waited += GW_STOP_POLL_MS) {
      usleep(GW_STOP_POLL_MS * 1000);
   }
}

void coding_gateway_stop(void) {
   atomic_store(&s_stopping, true);
   wait_in_flight();
   const char *alias = coding_gateway_alias();
   if (atomic_load(&s_in_flight) > 0 && alias != NULL) {
      mcp_bridge_interrupt(alias); /* their calls fail within about a second */
      wait_in_flight();
   }
}
