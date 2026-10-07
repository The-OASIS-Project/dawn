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
 * The WebUI's email work, off the lws thread.  A request is one task per
 * account; tasks run on a small pool, and the last one to finish builds the
 * reply and sends it to the session (no task ever waits on another).  An IMAP
 * task first gets its account's lease: if another caller holds it, the task
 * waits inside the lease without a worker, and the lease's hook queues it
 * again, already holding it, when it's handed over.
 *
 * Every request is answered: with its result, or with a refusal frame
 * (BUSY, SHUTTING_DOWN, SUPERSEDED) carrying the client's req.
 */

#ifndef WEBUI_EMAIL_EXEC_H
#define WEBUI_EMAIL_EXEC_H

#include <json-c/json.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tools/email_service.h"
#include "webui/email_wire.h"
#include "webui/webui_email_exec_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EMAIL_EXEC_OK 0
#define EMAIL_EXEC_FAILURE 1       /* bad request, or out of memory (not answered) */
#define EMAIL_EXEC_BUSY 2          /* refused: slot or user cap (answered BUSY) */
#define EMAIL_EXEC_SHUTTING_DOWN 3 /* refused: the executor is stopping (answered) */
#define EMAIL_EXEC_NO_WORKERS 4    /* refused: no worker could start (answered FAILED) */

#define EMAIL_EXEC_WORKERS 4
#define EMAIL_EXEC_USER_WORKERS (EMAIL_EXEC_WORKERS / 2) /* while others have work waiting */
#define EMAIL_EXEC_STACK_BYTES (1024 * 1024)
#define EMAIL_EXEC_MAX_TASKS 16               /* accounts one request may span */
#define EMAIL_EXEC_REQ_MAX EMAIL_WIRE_REQ_MAX /* the client's req, in bytes */
#define EMAIL_EXEC_VERB_MAX 48

/** What one account task gets. */
typedef struct {
   int user_id;
   int64_t account_id;
   const email_target_t *target; /* the account by id; lease_held when the task holds its lease */
   const atomic_bool *cancel;    /* also set as the thread's transfer cancel while it runs */
   email_err_t lease_err;        /* not EMAIL_ERR_NONE: the lease couldn't be had; record it */
   void *ctx;                    /* the request's context */
   int index;                    /* this task's account, 0..task_count-1 */
} email_exec_task_ctx_t;

/** Runs one account's part; writes its result into ctx at index (indexes never overlap). */
typedef void (*email_exec_op_fn)(const email_exec_task_ctx_t *task);

/**
 * Builds the reply's payload once every task is done; NULL sends nothing.  The
 * executor owns it: it adds "success" (true when absent) and "req", wraps it as
 * {"type":"<verb>_response","payload":…} and serializes it without escaping
 * '/'.  @p account_ids are the request's, in task order.
 */
typedef json_object *(*email_exec_finish_fn)(void *ctx, const int64_t *account_ids, int n);

/** Frees the request's context.  May run with the executor's lock held: takes no locks. */
typedef void (*email_exec_free_fn)(void *ctx);

typedef struct {
   uint32_t session_id;
   int user_id;
   email_exec_slot_t slot;
   const char *verb; /* "email_test_connection": replies are "<verb>_response" */
   const char *req;  /* the client's req, or NULL */
   int task_count;   /* 1..EMAIL_EXEC_MAX_TASKS */
   const int64_t *account_ids;
   /* Per task: takes the account's lease.  Read once, at submit: an account's
    * type (IMAP or Gmail) doesn't change. */
   const bool *is_imap;
   email_exec_op_fn op;
   email_exec_finish_fn finish;
   email_exec_free_fn free_ctx;
   void *ctx;
} email_exec_request_t;

/**
 * @brief Queue a request; the executor owns @p r->ctx from here, on every return
 * @return EMAIL_EXEC_OK, or a refusal the session has already been answered
 *         (BUSY, SHUTTING_DOWN, NO_WORKERS); EMAIL_EXEC_FAILURE (not
 *         answered) for a bad request or out of memory
 */
int webui_email_exec_submit(const email_exec_request_t *r);

/**
 * @brief Stop: refuse new work, cancel what's queued or waiting, finish what's
 *        running, join the workers.  Before the auth DB and email service go.
 */
void webui_email_exec_stop(void);

/** A refusal frame for @p verb (heap JSON): {success:false, error_code, error, req}. */
char *webui_email_exec_refusal_json(const char *verb, email_err_t code, const char *req);

/**
 * @brief The client's optional request id from a verb's payload
 * @return true when it is a string of at most EMAIL_EXEC_REQ_MAX bytes with no
 *         control characters (copied into @p out); a missing, longer or
 *         control-character one is ignored
 */
bool email_exec_payload_req(json_object *payload, char *out, size_t out_size);

/*
 * Where replies go (webui_email_exec_send.c; tests supply their own).  A reply
 * goes to the session that asked, by id, only while it's still that user's.
 */

/** The session, held, if it is still @p user_id's; NULL otherwise. */
void *webui_email_exec_session_open(uint32_t session_id, int user_id);
/** Queue @p json (taken: freed here or by the send queue) to an open session. */
void webui_email_exec_session_send(void *session, char *json);
/** Let go of a session from webui_email_exec_session_open. */
void webui_email_exec_session_close(void *session);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_EMAIL_EXEC_H */
