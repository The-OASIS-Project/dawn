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
 * What memory learned per consolidation batch, for the Memory panel's
 * Learned tab and the conversation marker: list_memory_batches (keyset
 * pages, newest first), undo_memory_batch and keep_memory_op (on a worker:
 * they wait out the user's extraction; the reply comes when it ends).  A
 * batch or op that isn't the caller's answers exactly as one that doesn't
 * exist.
 */

#include "webui/webui_memory_batches.h"

#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/outside_kind.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_consolidation_view.h"
#include "memory/memory_db_consolidation.h"
#include "webui/webui_handlers.h"
#include "webui/webui_internal.h"
#include "webui/webui_server.h"

#define ERR_NOT_FOUND "Not found"
#define ERR_LOAD "Couldn't load what memory learned. Try again in a moment."
#define ERR_UNDO_BUSY "Couldn't undo this right now. Try again in a moment."
#define ERR_KEEP_BUSY "Couldn't keep this right now. Try again in a moment."

bool webui_memory_batches_enabled(void) {
   return g_config.memory.enabled;
}

/* =============================================================================
 * The batch object
 * ============================================================================= */

static bool is(const char *a, const char *b) {
   return strcmp(a, b) == 0;
}

static json_object *id_or_null(int64_t id) {
   return id > 0 ? json_object_new_int64(id) : NULL;
}

static json_object *text_or_null(const char *text) {
   return text && text[0] ? json_object_new_string(text) : NULL;
}

/* What outside text @p k is, by name (outside_from), or NULL when none. */
static json_object *outside_from(int k) {
   static const char *const names[] = {
      [OUTSIDE_WEB] = "web",           [OUTSIDE_EMAIL] = "email",
      [OUTSIDE_RESEARCH] = "research", [OUTSIDE_MESSAGING] = "messaging",
      [OUTSIDE_DOCUMENT] = "document", [OUTSIDE_MCP] = "mcp",
      [OUTSIDE_OTHER] = "other",       [OUTSIDE_CALENDAR] = "calendar",
   };
   return outside_is_tainted(k) && k > 0 && k <= OUTSIDE_KIND_MAX && names[k]
              ? json_object_new_string(names[k])
              : NULL;
}

/* What a merge onto @p o's fact brought in: the batch's merged rows whose
 * keeper it is. */
static json_object *sources_of(const memory_consolidation_op_row_t *o,
                               const memory_consolidation_op_row_t *rows,
                               int n) {
   json_object *arr = NULL;
   for (int i = 0; o->fact_id > 0 && i < n; i++) {
      if (!is(rows[i].op, "merged") || is(rows[i].status, "refused") ||
          rows[i].other_fact_id != o->fact_id || &rows[i] == o) {
         continue;
      }
      if (!arr) {
         arr = json_object_new_array();
      }
      json_object *s = json_object_new_object();
      json_object_object_add(s, "fact_id", json_object_new_int64(rows[i].fact_id));
      json_object_object_add(s, "text", text_or_null(rows[i].text));
      json_object_array_add(arr, s);
   }
   return arr;
}

static json_object *op_json(const memory_consolidation_op_row_t *o,
                            const memory_consolidation_op_view_t *v,
                            const memory_consolidation_op_row_t *rows,
                            int n) {
   json_object *j = json_object_new_object();
   json_object_object_add(j, "op_id", json_object_new_int64(o->id));
   json_object_object_add(j, "op", json_object_new_string(v->op));
   json_object_object_add(j, "status", json_object_new_string(v->status));
   json_object_object_add(j, "outside", json_object_new_boolean(outside_is_tainted(o->outside)));
   json_object_object_add(j, "outside_from", outside_from(o->outside));
   json_object_object_add(j, "fact_id", id_or_null(o->fact_id));
   json_object_object_add(j, "relation_id", id_or_null(o->relation_id));
   json_object_object_add(j, "entity_id", id_or_null(o->entity_id));
   json_object_object_add(j, "text", text_or_null(o->text));
   json_object_object_add(j, "target_fact_id", v->link ? NULL : id_or_null(o->other_fact_id));
   json_object_object_add(j, "target_entity_id", v->link ? id_or_null(o->other_entity_id) : NULL);
   json_object_object_add(j, "target_text", text_or_null(o->target_text));
   json_object_object_add(j, "target_status", text_or_null(o->target_status));
   json_object *sources = sources_of(o, rows, n);
   if (sources) {
      json_object_object_add(j, "sources", sources);
   }
   /* Shown as text, never markup; the model's marked as its words. */
   json_object_object_add(j, "reason", text_or_null(o->reason));
   json_object_object_add(j, "reason_by", o->reason[0] ? text_or_null(o->reason_by) : NULL);
   json_object_object_add(j, "action", v->action ? json_object_new_string(v->action) : NULL);
   return j;
}

static json_object *batch_object(const memory_consolidation_batch_row_t *b,
                                 const memory_consolidation_op_row_t *rows,
                                 int n) {
   memory_consolidation_op_view_t *views = calloc((size_t)(n > 0 ? n : 1), sizeof(*views));
   if (!views) {
      return NULL;
   }
   memory_consolidation_batch_view_t bv;
   memory_consolidation_view(rows, n, views, &bv);
   json_object *ops = json_object_new_array();
   for (int i = 0; i < n; i++) {
      if (views[i].listed) {
         json_object_array_add(ops, op_json(&rows[i], &views[i], rows, n));
      }
   }
   free(views);
   json_object *j = json_object_new_object();
   json_object_object_add(j, "batch_id", json_object_new_int64(b->id));
   json_object_object_add(j, "conversation_id", id_or_null(b->conversation_id));
   json_object_object_add(j, "from_message_id", json_object_new_int64(b->msg_id_start));
   json_object_object_add(j, "after_message_id", json_object_new_int64(b->msg_id_end));
   json_object_object_add(j, "created_at", json_object_new_int64(b->created_at));
   json_object_object_add(j, "path", json_object_new_string(b->path));
   json_object_object_add(j, "status", json_object_new_string(bv.status));
   json_object_object_add(j, "outside", json_object_new_boolean(outside_is_tainted(b->outside)));
   json_object_object_add(j, "outside_from", outside_from(b->outside));
   json_object *c = json_object_new_object();
   for (int k = 0; k < MEMORY_CONSOLIDATION_COUNT_KINDS; k++) {
      json_object_object_add(c, memory_consolidation_count_kinds[k],
                             json_object_new_int(bv.counts[k]));
   }
   json_object_object_add(c, "refused", json_object_new_int(bv.refused));
   json_object_object_add(j, "counts", c);
   json_object_object_add(j, "ops", ops);
   return j;
}

/* @p b as the batch object, its ops read into @p rows (MEMORY_CONSOLIDATION_OPS_MAX). */
static json_object *batch_json_with(int user_id,
                                    const memory_consolidation_batch_row_t *b,
                                    memory_consolidation_op_row_t *rows) {
   int n = 0;
   if (memory_db_consolidation_ops_list(user_id, b->id, rows, MEMORY_CONSOLIDATION_OPS_MAX, &n) !=
       MEMORY_DB_SUCCESS) {
      return NULL;
   }
   json_object *j = batch_object(b, rows, n);
   memory_db_consolidation_ops_free(rows, n);
   return j;
}

/* webui_memory_batch_json, and whether the batch is no longer there. */
static json_object *batch_json(int user_id, int64_t batch_id, bool *gone) {
   memory_consolidation_batch_row_t b;
   const int rc = memory_db_consolidation_batch_get(user_id, batch_id, &b);
   *gone = rc == MEMORY_DB_NOT_FOUND;
   if (rc != MEMORY_DB_SUCCESS) {
      return NULL;
   }
   memory_consolidation_op_row_t *rows = calloc(MEMORY_CONSOLIDATION_OPS_MAX, sizeof(*rows));
   json_object *j = rows ? batch_json_with(user_id, &b, rows) : NULL;
   free(rows);
   return j;
}

json_object *webui_memory_batch_json(int user_id, int64_t batch_id) {
   bool gone = false;
   return batch_json(user_id, batch_id, &gone);
}

json_object *webui_memory_batch_notice(int user_id, int64_t batch_id) {
   bool gone = false;
   json_object *j = batch_json(user_id, batch_id, &gone);
   if (!j && gone) {
      j = json_object_new_object();
      json_object_object_add(j, "batch_id", json_object_new_int64(batch_id));
      json_object_object_add(j, "removed", json_object_new_boolean(1));
   }
   return j;
}

/* =============================================================================
 * Requests
 * ============================================================================= */

static int64_t int_field(json_object *obj, const char *key, bool *present) {
   json_object *v = NULL;
   const bool there = obj && json_object_object_get_ex(obj, key, &v) && v &&
                      (json_object_is_type(v, json_type_int) ||
                       json_object_is_type(v, json_type_double));
   if (present) {
      *present = there;
   }
   return there ? json_object_get_int64(v) : 0;
}

/* The page: up to @p limit batches (and as many bytes as a page holds) into
 * @p out, with has_more and next_before_batch_id.  FAILURE: the database. */
static int list_page(int user_id,
                     int64_t conversation_id,
                     int64_t before,
                     int limit,
                     json_object *out) {
   memory_consolidation_batch_row_t *b = calloc((size_t)limit + 1, sizeof(*b));
   memory_consolidation_op_row_t *rows = calloc(MEMORY_CONSOLIDATION_OPS_MAX, sizeof(*rows));
   int n = 0;
   if (!b || !rows ||
       memory_db_consolidation_batches_list(user_id, conversation_id, before, b, limit + 1, &n) !=
           MEMORY_DB_SUCCESS) {
      free(b);
      free(rows);
      return FAILURE;
   }
   json_object *batches = json_object_new_array();
   size_t bytes = 0;
   int shown = 0;
   int rc = SUCCESS;
   for (int i = 0; i < n && i < limit; i++) {
      json_object *j = batch_json_with(user_id, &b[i], rows);
      if (!j) {
         rc = FAILURE;
         break;
      }
      const size_t size = strlen(json_object_to_json_string_ext(j, JSON_C_TO_STRING_PLAIN));
      if (shown > 0 && bytes + size > WEBUI_MEMORY_BATCHES_PAGE_BYTES) {
         json_object_put(j);
         break; /* the rest on the next page */
      }
      bytes += size;
      json_object_array_add(batches, j);
      shown++;
   }
   const bool more = rc == SUCCESS && shown < n;
   json_object_object_add(out, "batches", batches);
   json_object_object_add(out, "has_more", json_object_new_boolean(more));
   if (more) {
      json_object_object_add(out, "next_before_batch_id", json_object_new_int64(b[shown - 1].id));
   }
   free(b);
   free(rows);
   return rc;
}

json_object *webui_memory_batches_list_payload(int user_id, json_object *request) {
   bool by_conv = false;
   bool by_before = false;
   bool by_limit = false;
   const int64_t conversation_id = int_field(request, "conversation_id", &by_conv);
   const int64_t before = int_field(request, "before_batch_id", &by_before);
   int64_t limit = int_field(request, "limit", &by_limit);
   limit = !by_limit || limit <= 0                  ? WEBUI_MEMORY_BATCHES_LIMIT
           : limit > WEBUI_MEMORY_BATCHES_LIMIT_MAX ? WEBUI_MEMORY_BATCHES_LIMIT_MAX
                                                    : limit;
   json_object *out = json_object_new_object();
   /* The request's form, echoed. */
   if (by_conv) {
      json_object_object_add(out, "conversation_id", json_object_new_int64(conversation_id));
   }
   json_object_object_add(out, "before_batch_id", by_before ? json_object_new_int64(before) : NULL);
   json_object_object_add(out, "limit", json_object_new_int((int)limit));
   const char *error = NULL;
   if (by_conv &&
       memory_db_consolidation_conversation_owned(user_id, conversation_id) != MEMORY_DB_SUCCESS) {
      error = ERR_NOT_FOUND; /* missing or another user's: the same */
   } else if (list_page(user_id, by_conv ? conversation_id : 0, before > 0 ? before : 0, (int)limit,
                        out) != SUCCESS) {
      error = ERR_LOAD;
   }
   if (error) {
      json_object_object_add(out, "batches", json_object_new_array());
      json_object_object_add(out, "has_more", json_object_new_boolean(false));
   }
   json_object_object_add(out, "error", error ? json_object_new_string(error) : NULL);
   return out;
}

static json_object *id_array(const int64_t *ids, int n) {
   json_object *arr = json_object_new_array();
   for (int i = 0; i < n; i++) {
      json_object_array_add(arr, json_object_new_int64(ids[i]));
   }
   return arr;
}

json_object *webui_memory_batch_outcome_payload(const memory_consolidation_outcome_t *io) {
   json_object *out = json_object_new_object();
   json_object_object_add(out, "batch_id", json_object_new_int64(io->batch_id));
   if (io->op_id > 0) {
      json_object_object_add(out, "op_id", json_object_new_int64(io->op_id));
   }
   json_object_object_add(out, "status",
                          json_object_new_string(memory_consolidation_status_name(io->status)));
   if (io->action == MEMORY_CONSOLIDATION_KEEP_OP) {
      json_object_object_add(out, "fact_id", id_or_null(io->fact_id));
   } else {
      json_object_object_add(out, "undone", id_array(io->undone, io->n_undone));
      json_object_object_add(out, "stale", id_array(io->stale, io->n_stale));
   }
   json_object_object_add(out, "error", io->error ? json_object_new_string(io->error) : NULL);
   return out;
}

/* =============================================================================
 * Handlers
 * ============================================================================= */

static void send_payload(ws_connection_t *conn, const char *type, json_object *payload) {
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string(type));
   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}

void handle_list_memory_batches(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }
   send_payload(conn, "list_memory_batches_response",
                webui_memory_batches_list_payload(conn->auth_user_id, payload));
}

/* The reply's session, looked up again when the worker ends. */
typedef struct {
   uint32_t session_id;
} batch_reply_t;

static void reply_when_done(const memory_consolidation_outcome_t *io, void *ctx) {
   batch_reply_t *reply = ctx;
   json_object *frame = json_object_new_object();
   json_object_object_add(frame, "type",
                          json_object_new_string(io->action == MEMORY_CONSOLIDATION_KEEP_OP
                                                     ? "keep_memory_op_response"
                                                     : "undo_memory_batch_response"));
   json_object_object_add(frame, "payload", webui_memory_batch_outcome_payload(io));
   session_t *session = session_get(reply->session_id); /* NULL once disconnected */
   if (session) {
      webui_send_session_json(session,
                              json_object_to_json_string_ext(frame, JSON_C_TO_STRING_PLAIN));
      session_release(session);
   }
   json_object_put(frame);
   free(reply);
}

/* Start @p action; a reply now when it can't start. */
static void start_action(ws_connection_t *conn,
                         memory_consolidation_action_t action,
                         struct json_object *payload) {
   memory_consolidation_outcome_t io;
   memset(&io, 0, sizeof(io));
   io.action = action;
   io.user_id = conn->auth_user_id;
   io.batch_id = int_field(payload, "batch_id", NULL);
   io.op_id = int_field(payload, "op_id", NULL);
   io.status = MEMORY_CONSOLIDATION_ERROR;
   const char *type = action == MEMORY_CONSOLIDATION_KEEP_OP ? "keep_memory_op_response"
                                                             : "undo_memory_batch_response";
   if (io.batch_id <= 0 || (action != MEMORY_CONSOLIDATION_UNDO_BATCH && io.op_id <= 0)) {
      io.error = ERR_NOT_FOUND;
      send_payload(conn, type, webui_memory_batch_outcome_payload(&io));
      return;
   }
   batch_reply_t *reply = conn->session ? calloc(1, sizeof(*reply)) : NULL;
   int rc = FAILURE;
   if (reply) {
      reply->session_id = conn->session->session_id;
      rc = memory_consolidation_start(action, io.user_id, io.batch_id, io.op_id, reply_when_done,
                                      reply);
   }
   if (rc != SUCCESS) {
      free(reply);
      io.error = action == MEMORY_CONSOLIDATION_KEEP_OP ? ERR_KEEP_BUSY : ERR_UNDO_BUSY;
      send_payload(conn, type, webui_memory_batch_outcome_payload(&io));
   }
}

void handle_undo_memory_batch(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }
   /* An op_id that is there but not a number names no op: never the whole batch. */
   json_object *v = NULL;
   const bool named = payload && json_object_object_get_ex(payload, "op_id", &v);
   if (named && !(v && json_object_is_type(v, json_type_int))) {
      memory_consolidation_outcome_t io;
      memset(&io, 0, sizeof(io));
      io.action = MEMORY_CONSOLIDATION_UNDO_OP;
      io.batch_id = int_field(payload, "batch_id", NULL);
      io.status = MEMORY_CONSOLIDATION_ERROR;
      io.error = ERR_NOT_FOUND;
      send_payload(conn, "undo_memory_batch_response", webui_memory_batch_outcome_payload(&io));
      return;
   }
   start_action(conn, named ? MEMORY_CONSOLIDATION_UNDO_OP : MEMORY_CONSOLIDATION_UNDO_BATCH,
                payload);
}

void handle_keep_memory_op(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }
   start_action(conn, MEMORY_CONSOLIDATION_KEEP_OP, payload);
}
