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
 * The mail panel's move verbs: email_archive, email_trash and email_undo
 * (webui_email_panel.h).  Checked on the lws thread like the other verbs
 * (webui_email_panel.c), run on the executor's MOVE slot.
 */

#include <json-c/json.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

#include "tools/email_service.h"
#include "tools/email_undo.h"
#include "webui/email_wire.h"
#include "webui/webui_email_exec.h"
#include "webui/webui_email_panel.h"
#include "webui/webui_email_panel_internal.h"

/* =============================================================================
 * email_archive / email_trash / email_undo
 *
 * Both run on the MOVE slot, queued in order and never replaced: a move that
 * started finishes on the server, and the service tells every tab of the user
 * what changed (email_changed), whether or not this reply is delivered.
 * ============================================================================= */

/* A move or undo request's account: the user's enabled, writable account, with
 * the request's session.  Answers (and returns NULL) otherwise. */
static const panel_acct_t *move_account(ws_connection_t *conn,
                                        const char *verb,
                                        json_object *payload,
                                        const char *req,
                                        const panel_acct_t *accts,
                                        int n_accts,
                                        uint32_t *session_id) {
   bool valid = false, have_session = false;
   const panel_acct_t *a = email_panel_get_account(payload, accts, n_accts, &valid);
   *session_id = email_panel_session_id_of(conn, &have_session);
   email_err_t e = EMAIL_ERR_NONE;
   if (!valid)
      e = EMAIL_ERR_INVALID_REQUEST;
   else if (!a)
      e = EMAIL_ERR_ACCOUNT_NOT_FOUND;
   else if (!have_session)
      e = EMAIL_ERR_FAILED;
   else if (a->read_only)
      e = EMAIL_ERR_READ_ONLY; /* the service checks again (under the lease, for IMAP) */
   if (e == EMAIL_ERR_NONE)
      return a;
   email_panel_reply_error(conn, verb, e, req);
   return NULL;
}

/* The array member @p key of 1..EMAIL_MOVE_MAX_IDS strings, each up to @p max
 * bytes, copied into rows of @p stride bytes. */
static bool get_id_list(json_object *payload,
                        const char *key,
                        size_t max,
                        char *out,
                        size_t stride,
                        int *n) {
   json_object *arr = NULL;
   *n = 0;
   if (!payload || !json_object_object_get_ex(payload, key, &arr) ||
       !json_object_is_type(arr, json_type_array))
      return false;
   const size_t len = json_object_array_length(arr);
   if (len < 1 || len > EMAIL_MOVE_MAX_IDS)
      return false;
   for (size_t i = 0; i < len; i++) {
      json_object *v = json_object_array_get_idx(arr, i);
      if (!email_panel_text_ok(v, max) || (size_t)json_object_get_string_len(v) >= stride)
         return false;
      memcpy(out + i * stride, json_object_get_string(v),
             (size_t)json_object_get_string_len(v) + 1);
   }
   *n = (int)len;
   return true;
}

typedef struct {
   email_move_kind_t kind;
   int n;
   char ids[EMAIL_MOVE_MAX_IDS][PANEL_MSG_ID_MAX + 1];
   int rc;
   email_err_t err;
   email_move_result_t *results; /* n, allocated on the worker */
} move_ctx_t;

static void move_op(const email_exec_task_ctx_t *t) {
   move_ctx_t *c = (move_ctx_t *)t->ctx;
   c->rc = EMAIL_RC_FAILURE;
   if (t->lease_err != EMAIL_ERR_NONE) {
      c->err = t->lease_err;
      return;
   }
   c->results = calloc((size_t)c->n, sizeof(*c->results));
   if (!c->results) {
      c->err = EMAIL_ERR_FAILED;
      return;
   }
   const char *ptrs[EMAIL_MOVE_MAX_IDS];
   for (int i = 0; i < c->n; i++)
      ptrs[i] = c->ids[i];
   c->rc = email_service_move(t->user_id, t->target, ptrs, c->n, c->kind, true, c->results,
                              &c->err);
}

static json_object *move_finish(void *ctx, const int64_t *account_ids, int n) {
   (void)account_ids;
   (void)n;
   move_ctx_t *c = (move_ctx_t *)ctx;
   /* A call that failed as a whole may still have moved some: those are told. */
   bool any_moved = false;
   for (int i = 0; c->results && i < c->n; i++)
      any_moved = any_moved || c->results[i].outcome != EMAIL_MOVE_FAILED;
   if (c->rc != EMAIL_RC_OK && !any_moved)
      return email_panel_error_payload(c->err == EMAIL_ERR_NONE ? EMAIL_ERR_FAILED : c->err);
   const char *ids[EMAIL_MOVE_MAX_IDS];
   for (int i = 0; i < c->n; i++)
      ids[i] = c->ids[i];
   return email_wire_move_payload(ids, c->results, c->n);
}

static void move_free(void *ctx) {
   move_ctx_t *c = (move_ctx_t *)ctx;
   if (!c)
      return;
   free(c->results);
   free(c);
}

static void handle_move(ws_connection_t *conn, json_object *payload, email_move_kind_t kind) {
   const char *verb = kind == EMAIL_MOVE_TRASH ? "email_trash" : "email_archive";
   char req_buf[EMAIL_EXEC_REQ_MAX + 1];
   const char *req = NULL;
   if (!email_panel_verb_start(conn, verb, payload, req_buf, &req))
      return;

   move_ctx_t *c = calloc(1, sizeof(*c));
   if (!c) {
      email_panel_reply_error(conn, verb, EMAIL_ERR_FAILED, req);
      return;
   }
   c->kind = kind;
   if (!get_id_list(payload, "message_ids", PANEL_MSG_ID_MAX, c->ids[0], sizeof(c->ids[0]),
                    &c->n)) {
      free(c);
      email_panel_reply_error(conn, verb, EMAIL_ERR_INVALID_REQUEST, req);
      return;
   }
   panel_acct_t accts[EMAIL_MAX_ACCOUNTS];
   const int n_accts = email_panel_load_accounts(conn->auth_user_id, accts);
   uint32_t session_id = 0;
   const panel_acct_t *a = move_account(conn, verb, payload, req, accts, n_accts, &session_id);
   if (!a) {
      free(c);
      return;
   }
   for (int i = 0; i < c->n; i++) {
      if (!email_panel_msg_id_ok(c->ids[i], a->is_imap)) {
         free(c);
         email_panel_reply_error(conn, verb, EMAIL_ERR_INVALID_REQUEST, req);
         return;
      }
   }
   const int64_t id = a->id;
   const bool is_imap = a->is_imap;
   const email_exec_request_t r = {
      .session_id = session_id,
      .user_id = conn->auth_user_id,
      .slot = EMAIL_EXEC_SLOT_MOVE,
      .verb = verb,
      .req = req,
      .task_count = 1,
      .account_ids = &id,
      .is_imap = &is_imap,
      .op = move_op,
      .finish = move_finish,
      .free_ctx = move_free,
      .ctx = c,
   };
   email_panel_submit(conn, &r);
}

void handle_email_archive(ws_connection_t *conn, json_object *payload) {
   handle_move(conn, payload, EMAIL_MOVE_ARCHIVE);
}

void handle_email_trash(ws_connection_t *conn, json_object *payload) {
   handle_move(conn, payload, EMAIL_MOVE_TRASH);
}

typedef struct {
   bool is_imap;
   int n;
   char tokens[EMAIL_MOVE_MAX_IDS][EMAIL_UNDO_TOKEN_LEN + 1];
   email_err_t err; /* the whole call's, when nothing ran */
   bool claimed[EMAIL_MOVE_MAX_IDS];
   int at[EMAIL_MOVE_MAX_IDS];   /* token i's place in recs/results, when claimed */
   email_undo_result_t *results; /* the claimed ones', allocated on the worker */
} undo_ctx_t;

/* The tokens are claimed here, after the lease, so a request cancelled before
 * it starts holds none; each goes back by its value: finished, or released when
 * its undo couldn't run this time. */
static void undo_op(const email_exec_task_ctx_t *t) {
   undo_ctx_t *c = (undo_ctx_t *)t->ctx;
   if (t->lease_err != EMAIL_ERR_NONE) {
      c->err = t->lease_err;
      return;
   }
   email_undo_rec_t *recs = calloc((size_t)c->n, sizeof(*recs));
   c->results = calloc((size_t)c->n, sizeof(*c->results));
   if (!recs || !c->results) {
      free(recs);
      c->err = EMAIL_ERR_FAILED;
      return;
   }
   int m = 0;
   for (int i = 0; i < c->n; i++) {
      c->claimed[i] = email_undo_claim(t->user_id, t->account_id, c->tokens[i], &recs[m]);
      if (c->claimed[i])
         c->at[i] = m++;
   }
   if (m > 0) {
      email_err_t err = EMAIL_ERR_NONE;
      email_service_undo(t->user_id, t->target, recs, m, c->results, &err);
      for (int i = 0; i < c->n; i++) {
         if (!c->claimed[i])
            continue;
         if (c->results[c->at[i]].retry)
            email_undo_release(t->user_id, c->tokens[i]);
         else
            email_undo_finish(t->user_id, c->tokens[i]);
      }
   }
   sodium_memzero(recs, (size_t)c->n * sizeof(*recs));
   free(recs);
}

static json_object *undo_finish(void *ctx, const int64_t *account_ids, int n) {
   undo_ctx_t *c = (undo_ctx_t *)ctx;
   if (!c->results || n != 1)
      return email_panel_error_payload(c->err == EMAIL_ERR_NONE ? EMAIL_ERR_FAILED : c->err);
   const char *tokens[EMAIL_MOVE_MAX_IDS];
   const email_undo_result_t *results[EMAIL_MOVE_MAX_IDS];
   for (int i = 0; i < c->n; i++) {
      tokens[i] = c->tokens[i];
      results[i] = c->claimed[i] ? &c->results[c->at[i]] : NULL;
   }
   return email_wire_undo_payload(account_ids[0], tokens, results, c->n, !c->is_imap);
}

static void undo_free(void *ctx) {
   undo_ctx_t *c = (undo_ctx_t *)ctx;
   if (!c)
      return;
   free(c->results);
   free(c);
}

void handle_email_undo(ws_connection_t *conn, json_object *payload) {
   const char *verb = "email_undo";
   char req_buf[EMAIL_EXEC_REQ_MAX + 1];
   const char *req = NULL;
   if (!email_panel_verb_start(conn, verb, payload, req_buf, &req))
      return;

   undo_ctx_t *c = calloc(1, sizeof(*c));
   if (!c) {
      email_panel_reply_error(conn, verb, EMAIL_ERR_FAILED, req);
      return;
   }
   bool ok = get_id_list(payload, "undo", EMAIL_UNDO_TOKEN_LEN, c->tokens[0], sizeof(c->tokens[0]),
                         &c->n);
   for (int i = 0; ok && i < c->n; i++) {
      ok = email_wire_undo_token_ok(c->tokens[i]);
      for (int j = 0; ok && j < i; j++)
         ok = strcmp(c->tokens[i], c->tokens[j]) != 0; /* a repeat would read as expired */
   }
   if (!ok) {
      free(c);
      email_panel_reply_error(conn, verb, EMAIL_ERR_INVALID_REQUEST, req);
      return;
   }
   panel_acct_t accts[EMAIL_MAX_ACCOUNTS];
   const int n_accts = email_panel_load_accounts(conn->auth_user_id, accts);
   uint32_t session_id = 0;
   const panel_acct_t *a = move_account(conn, verb, payload, req, accts, n_accts, &session_id);
   if (!a) {
      free(c);
      return;
   }
   c->is_imap = a->is_imap;
   const int64_t id = a->id;
   const bool is_imap = a->is_imap;
   const email_exec_request_t r = {
      .session_id = session_id,
      .user_id = conn->auth_user_id,
      .slot = EMAIL_EXEC_SLOT_MOVE,
      .verb = verb,
      .req = req,
      .task_count = 1,
      .account_ids = &id,
      .is_imap = &is_imap,
      .op = undo_op,
      .finish = undo_finish,
      .free_ctx = undo_free,
      .ctx = c,
   };
   email_panel_submit(conn, &r);
}
