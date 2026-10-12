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
 * The Memory panel's merged and replaced facts: list_memory_superseded lists
 * them (newest first, optionally by text), restore_memory_fact brings one
 * back.  The connection's own user only.
 */

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"
#include "memory/memory_db.h"
#include "webui/webui_handlers.h"
#include "webui/webui_internal.h"
#include "webui/webui_server.h"

/* Superseded facts one listing returns. */
#define SUPERSEDED_LIST_MAX 50

static void send_payload(ws_connection_t *conn, const char *type, json_object *payload) {
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string(type));
   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}

void handle_list_memory_superseded(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }
   const char *query = NULL;
   json_object *q = NULL;
   if (payload && json_object_object_get_ex(payload, "query", &q) &&
       json_object_is_type(q, json_type_string)) {
      query = json_object_get_string(q);
   }
   json_object *out = json_object_new_object();
   /* Echoed, so a reply to an older search is told apart from the newest. */
   json_object_object_add(out, "query", json_object_new_string(query ? query : ""));
   memory_superseded_fact_t *rows = calloc(SUPERSEDED_LIST_MAX, sizeof(*rows));
   int n = 0;
   const bool ok = rows &&
                   memory_db_fact_list_superseded(conn->auth_user_id, 0, query, rows,
                                                  SUPERSEDED_LIST_MAX, &n) == MEMORY_DB_SUCCESS;
   json_object_object_add(out, "success", json_object_new_boolean(ok));
   if (ok) {
      json_object *facts = json_object_new_array();
      for (int i = 0; i < n; i++) {
         json_object *f = json_object_new_object();
         json_object_object_add(f, "id", json_object_new_int64(rows[i].id));
         json_object_object_add(f, "text", json_object_new_string(rows[i].text));
         json_object_object_add(f, "keeper_id", json_object_new_int64(rows[i].keeper_id));
         json_object_object_add(f, "keeper_text", json_object_new_string(rows[i].keeper_text));
         json_object_object_add(f, "replaced", json_object_new_boolean(rows[i].replaced));
         json_object_object_add(f, "superseded_at",
                                json_object_new_int64((int64_t)rows[i].superseded_at));
         json_object_array_add(facts, f);
      }
      json_object_object_add(out, "facts", facts);
   } else {
      json_object_object_add(out, "error", json_object_new_string("Couldn't load merged memories"));
   }
   free(rows);
   send_payload(conn, "list_memory_superseded_response", out);
}

void handle_restore_memory_fact(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }
   json_object *id_obj = NULL;
   const int64_t fact_id = payload && json_object_object_get_ex(payload, "fact_id", &id_obj)
                               ? json_object_get_int64(id_obj)
                               : 0;
   json_object *out = json_object_new_object();
   json_object_object_add(out, "fact_id", json_object_new_int64(fact_id));
   memory_restore_result_t rr = { 0 };
   const int rc = fact_id > 0 ? memory_db_fact_restore(conn->auth_user_id, fact_id, &rr)
                              : MEMORY_DB_NOT_FOUND;
   json_object_object_add(out, "success", json_object_new_boolean(rc == MEMORY_DB_SUCCESS));
   /* An older value back beside the newer one: the user may want to forget one. */
   json_object_object_add(out, "both_current", json_object_new_boolean(rr.replaced));
   if (rc == MEMORY_DB_SUCCESS) {
      OLOG_INFO("WebUI: User %d restored memory fact %lld", conn->auth_user_id, (long long)fact_id);
   } else {
      json_object_object_add(out, "error",
                             json_object_new_string(rc == MEMORY_DB_NOT_FOUND
                                                        ? "That memory isn't merged any more"
                                                        : "Couldn't restore the memory"));
   }
   send_payload(conn, "restore_memory_fact_response", out);
}
