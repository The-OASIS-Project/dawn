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
 * The mail panel's verbs (webui_email_panel.h), but for the moves
 * (webui_email_panel_move.c).  A request is checked here, on the lws thread,
 * against its limits and the user's own enabled accounts, and anything it
 * carries that isn't plainly valid is refused before the service sees it.  The
 * work then runs on the email executor, one task per account; the last task's
 * finish builds the reply.
 */

#include "webui/webui_email_panel.h"

#include <ctype.h>
#include <json-c/json.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "logging.h"
#include "tools/email_db.h"
#include "tools/email_parse.h"
#include "tools/email_service.h"
#include "tools/gmail_client.h"
#include "tools/tool_registry.h"
#include "webui/email_cursor.h"
#include "webui/email_wire.h"
#include "webui/webui_email_exec.h"
#include "webui/webui_email_panel_internal.h"

#define PANEL_LIMIT_DEFAULT 25
#define PANEL_LIMIT_MAX EMAIL_MAX_FETCH_RESULTS
#define PANEL_FOLDER_MAX 127

_Static_assert(EMAIL_CURSOR_ACCOUNTS >= EMAIL_MAX_ACCOUNTS, "a cursor holds every account");
_Static_assert(EMAIL_EXEC_MAX_TASKS >= EMAIL_MAX_ACCOUNTS, "a request may span every account");

bool webui_email_client_enabled(void) {
   const tool_metadata_t *m = tool_registry_lookup("email");
   return m && m->is_available && m->is_available();
}

/* =============================================================================
 * Replies from the lws thread, and request checks
 * ============================================================================= */

void email_panel_reply(ws_connection_t *conn,
                       const char *verb,
                       json_object *payload,
                       const char *req) {
   json_object *frame = json_object_new_object();
   if (!frame || !payload) {
      json_object_put(frame);
      json_object_put(payload);
      return;
   }
   char type[EMAIL_EXEC_VERB_MAX + 16];
   snprintf(type, sizeof(type), "%s_response", verb);
   json_object *ignored;
   if (!json_object_object_get_ex(payload, "success", &ignored))
      json_object_object_add(payload, "success", json_object_new_boolean(1));
   if (req)
      json_object_object_add(payload, "req", json_object_new_string(req));
   json_object_object_add(frame, "type", json_object_new_string(type));
   json_object_object_add(frame, "payload", payload);
   send_json_response(conn, frame);
   json_object_put(frame);
}

json_object *email_panel_error_payload(email_err_t code) {
   json_object *p = json_object_new_object();
   if (!p)
      return NULL;
   json_object_object_add(p, "success", json_object_new_boolean(0));
   json_object_object_add(p, "error_code", json_object_new_string(email_error_name(code)));
   json_object_object_add(p, "error", json_object_new_string(email_wire_error_text(code)));
   return p;
}

void email_panel_reply_error(ws_connection_t *conn,
                             const char *verb,
                             email_err_t code,
                             const char *req) {
   email_panel_reply(conn, verb, email_panel_error_payload(code), req);
}

/* A bool member: @p def when absent; false when it isn't a bool. */
static bool get_bool(json_object *payload, const char *key, bool def, bool *out) {
   json_object *v;
   *out = def;
   if (!payload || !json_object_object_get_ex(payload, key, &v))
      return true;
   if (!json_object_is_type(v, json_type_boolean))
      return false;
   *out = json_object_get_boolean(v);
   return true;
}

/* An integer member in [lo, hi]: @p def when absent; false otherwise. */
static bool get_int(json_object *payload,
                    const char *key,
                    int64_t lo,
                    int64_t hi,
                    int64_t def,
                    int64_t *out) {
   json_object *v;
   *out = def;
   if (!payload || !json_object_object_get_ex(payload, key, &v))
      return true;
   if (!json_object_is_type(v, json_type_int))
      return false;
   const int64_t n = json_object_get_int64(v);
   if (n < lo || n > hi)
      return false;
   *out = n;
   return true;
}

/* A string of 1..max bytes with no control characters or embedded NUL. */
bool email_panel_text_ok(json_object *v, size_t max) {
   if (!v || !json_object_is_type(v, json_type_string))
      return false;
   const char *s = json_object_get_string(v);
   const size_t len = (size_t)json_object_get_string_len(v);
   if (len == 0 || len > max || strlen(s) != len)
      return false;
   for (size_t i = 0; i < len; i++) {
      const unsigned char c = (unsigned char)s[i];
      if (c < 0x20 || c == 0x7f)
         return false;
   }
   return true;
}

/* A string member copied into @p out (1..max bytes, printable): @p present says if it was there. */
static bool get_text(json_object *payload,
                     const char *key,
                     size_t max,
                     char *out,
                     size_t size,
                     bool *present) {
   json_object *v;
   out[0] = '\0';
   *present = false;
   if (!payload || !json_object_object_get_ex(payload, key, &v))
      return true;
   if (!email_panel_text_ok(v, max) || (size_t)json_object_get_string_len(v) >= size)
      return false;
   *present = true;
   memcpy(out, json_object_get_string(v), (size_t)json_object_get_string_len(v) + 1);
   return true;
}

int email_panel_load_accounts(int user_id, panel_acct_t *out) {
   email_account_t accounts[EMAIL_MAX_ACCOUNTS];
   const int n = email_service_list_accounts(user_id, accounts, EMAIL_MAX_ACCOUNTS);
   int count = 0;
   for (int i = 0; i < n && count < EMAIL_MAX_ACCOUNTS; i++) {
      if (!accounts[i].enabled)
         continue;
      out[count].id = accounts[i].id;
      out[count].is_imap = email_service_account_uses_lease(&accounts[i]);
      out[count].read_only = accounts[i].read_only;
      count++;
   }
   sodium_memzero(accounts, sizeof(accounts));
   return count;
}

/* A message id of the account's kind, checked before it goes any further:
 * IMAP "folder:uid" with a valid folder, or a Gmail hex id. */
bool email_panel_msg_id_ok(const char *id, bool is_imap) {
   if (!is_imap)
      return gmail_message_id_valid(id);
   char folder[PANEL_FOLDER_MAX + 1];
   uint32_t uid = 0;
   return email_imap_id_parse(id, folder, sizeof(folder), &uid, NULL) &&
          email_service_validate_folder_name(folder);
}

static const panel_acct_t *find_acct(const panel_acct_t *accts, int n, int64_t id) {
   for (int i = 0; i < n; i++) {
      if (accts[i].id == id)
         return &accts[i];
   }
   return NULL;
}

/* The request's session, or NULL. */
uint32_t email_panel_session_id_of(ws_connection_t *conn, bool *ok) {
   session_t *s = conn_get_session(conn);
   *ok = s != NULL;
   return s ? s->session_id : 0;
}

/* Submit; on a submit that wasn't answered, answer it here. */
void email_panel_submit(ws_connection_t *conn, const email_exec_request_t *r) {
   if (webui_email_exec_submit(r) == EMAIL_EXEC_FAILURE)
      email_panel_reply_error(conn, r->verb, EMAIL_ERR_FAILED, r->req);
}

/* The verb's common start: logged in, email on, the request's req. */
bool email_panel_verb_start(ws_connection_t *conn,
                            const char *verb,
                            json_object *payload,
                            char *req,
                            const char **req_out) {
   if (!conn_require_auth(conn))
      return false;
   *req_out = email_exec_payload_req(payload, req, EMAIL_EXEC_REQ_MAX + 1) ? req : NULL;
   if (!webui_email_client_enabled()) {
      email_panel_reply_error(conn, verb, EMAIL_ERR_UNAVAILABLE, *req_out);
      return false;
   }
   return true;
}

/* =============================================================================
 * email_list / email_search
 * ============================================================================= */

typedef struct {
   email_cursor_pos_t from;
   email_err_t err;
   email_summary_t *rows;
   int row_count;
   int split;
   bool more;
   uint32_t next_before;
   uint32_t next_v;  /* the mailbox epoch this fetch saw */
   uint32_t next_f;  /* IMAP: the folder this fetch read (email_cursor_folder_hash) */
   int inbox_unread; /* -1 unknown */
} list_acct_t;

typedef struct {
   bool search;
   char folder[PANEL_FOLDER_MAX + 1];
   bool unread_only;
   char query[EMAIL_SEARCH_TEXT_MAX + 1];
   int limit;
   bool want_counts;
   uint64_t filter;
   int n;
   list_acct_t acct[EMAIL_MAX_ACCOUNTS];
   int missing_count; /* ids asked for that aren't the user's enabled accounts */
   int64_t missing[EMAIL_MAX_ACCOUNTS];
} list_ctx_t;

/* What one page fetch takes and gives beyond its rows. */
typedef struct {
   const char *token;    /* the provider's page ("" = the first) */
   time_t at_or_before;  /* Gmail: rows dated at or before this (0 = no bound) */
   bool want_unread;     /* the inbox's unread count (list, first page) */
   int inbox_unread;     /* out: -1 unknown */
   uint32_t uidvalidity; /* out, IMAP: the mailbox epoch seen */
   char folder[128];     /* out, IMAP: the folder read, as the server resolved it */
} page_req_t;

/* One page from one account. */
static email_err_t fetch_page(const list_ctx_t *c,
                              const email_exec_task_ctx_t *t,
                              page_req_t *pr,
                              email_summary_t *out,
                              int max,
                              int *count,
                              char *npt) {
   email_err_t err = EMAIL_ERR_FAILED;
   int rc;
   *count = 0;
   npt[0] = '\0';
   pr->inbox_unread = -1;
   pr->uidvalidity = 0;
   pr->folder[0] = '\0';
   if (!c->search) {
      email_page_ext_t ext = { .want_inbox_unread = pr->want_unread,
                               .at_or_before = pr->at_or_before };
      rc = email_service_recent(t->user_id, NULL, c->folder, max, c->unread_only, pr->token, out,
                                max, count, npt, EMAIL_PAGE_TOKEN_LEN, &ext, t->target, &err);
      pr->inbox_unread = ext.inbox_unread;
      pr->uidvalidity = ext.uidvalidity;
      snprintf(pr->folder, sizeof(pr->folder), "%s", ext.imap_folder);
   } else {
      email_search_params_t p;
      memset(&p, 0, sizeof(p));
      snprintf(p.text, sizeof(p.text), "%s", c->query);
      snprintf(p.folder, sizeof(p.folder), "%s", c->folder);
      snprintf(p.page_token, sizeof(p.page_token), "%s", pr->token ? pr->token : "");
      p.unread_only = c->unread_only;
      p.gmail_at_or_before = (int64_t)pr->at_or_before;
      email_search_report_t rep;
      rc = email_service_search(t->user_id, NULL, &p, out, max, count, npt, EMAIL_PAGE_TOKEN_LEN,
                                NULL, 0, t->target, &rep);
      err = rep.err;
      pr->uidvalidity = rep.uidvalidity;
      snprintf(pr->folder, sizeof(pr->folder), "%s", rep.imap_folder);
   }
   if (rc == EMAIL_RC_OK)
      return EMAIL_ERR_NONE;
   if (rc == EMAIL_RC_INVALID_PAGE_TOKEN)
      return EMAIL_ERR_CURSOR_STALE;
   return err == EMAIL_ERR_NONE ? EMAIL_ERR_FAILED : err;
}

static int by_uid_desc(const void *a, const void *b) {
   const uint32_t ua = ((const email_summary_t *)a)->uid;
   const uint32_t ub = ((const email_summary_t *)b)->uid;
   return ua < ub ? 1 : (ua > ub ? -1 : 0);
}

static void list_imap(list_ctx_t *c, list_acct_t *a, const email_exec_task_ctx_t *t) {
   char tok[EMAIL_PAGE_TOKEN_LEN] = "";
   if (a->from.before_uid &&
       !email_imap_page_token_format(a->from.before_uid, a->from.uidvalidity, tok, sizeof(tok))) {
      a->err = EMAIL_ERR_CURSOR_STALE;
      return;
   }
   char npt[EMAIL_PAGE_TOKEN_LEN];
   int count = 0;
   page_req_t pr = { .token = tok, .want_unread = c->want_counts };
   a->err = fetch_page(c, t, &pr, a->rows, c->limit, &count, npt);
   a->inbox_unread = pr.inbox_unread;
   if (a->err != EMAIL_ERR_NONE)
      return;
   /* UIDs only mean something in the folder they came from: "all" may name a
    * different folder now than when the cursor was made (the server's \All
    * folder found, or lost). */
   if (!email_cursor_imap_folder_ok(&a->from, pr.folder)) {
      a->err = EMAIL_ERR_CURSOR_STALE;
      return;
   }
   if (pr.folder[0])
      a->next_f = email_cursor_folder_hash(pr.folder);
   /* The fetch's order is the server's: the merge needs UID-descending. */
   qsort(a->rows, (size_t)count, sizeof(a->rows[0]), by_uid_desc);
   a->row_count = count;
   /* Pinned even when this is the last page, so a rebuilt mailbox reads as stale. */
   a->next_v = pr.uidvalidity;
   a->more = npt[0] != '\0';
   uint32_t npt_v = 0;
   if (a->more && !email_imap_page_token_parse(npt, &a->next_before, &npt_v))
      a->more = false;
   if (npt_v)
      a->next_v = npt_v;
}

/* Gmail rows a fetch may hold: a page, plus what a resume second can repeat. */
#define GMAIL_FETCH_ROWS(limit) ((limit) + EMAIL_CURSOR_SEEN_MAX)
/* Provider pages one Gmail fetch reads at most, topping up a short page.  A
 * second holding more messages than these reach is stepped past (rare: bulk
 * imports). */
#define GMAIL_FETCH_PAGES 3

/* Gmail pages by date, not by page token: the provider's tokens are offsets, so
 * mail arriving or leaving between requests would shift them.  Rows dated after
 * the resume point are left out by the query, and the ones of that second
 * already emitted by the filter. */
static void list_gmail(list_ctx_t *c, list_acct_t *a, const email_exec_task_ctx_t *t) {
   const int cap = GMAIL_FETCH_ROWS(c->limit);
   char tok[EMAIL_PAGE_TOKEN_LEN] = "";
   int have = 0;
   bool more = false;
   int skip_left = email_cursor_gmail_skip(&a->from);
   for (int pages = 0; pages < GMAIL_FETCH_PAGES && have < c->limit; pages++) {
      char npt[EMAIL_PAGE_TOKEN_LEN];
      int count = 0;
      /* A page and the resume second's repeats, less what's already kept: a
       * top-up asks for little unless the filter dropped rows (a crowded second,
       * whose next rows lie past what was fetched). */
      const int want = cap - have;
      page_req_t pr = { .token = tok,
                        .at_or_before = a->from.next_date,
                        .want_unread = pages == 0 && c->want_counts };
      const email_err_t err = fetch_page(c, t, &pr, a->rows + have, want, &count, npt);
      if (pages == 0)
         a->inbox_unread = pr.inbox_unread;
      if (err != EMAIL_ERR_NONE) {
         if (pages == 0)
            a->err = err;
         /* A failed top-up isn't the account failing: the cursor resumes by date. */
         break;
      }
      have += email_cursor_gmail_filter(a->rows + have, count, &a->from, &skip_left);
      more = npt[0] != '\0';
      if (!more)
         break;
      snprintf(tok, sizeof(tok), "%s", npt);
   }
   /* Paging by date assumes the provider lists newest first; if a fetch comes
    * back otherwise, sorting keeps the position taken from it consistent. */
   static atomic_bool s_order_logged;
   if (email_cursor_gmail_order(a->rows, have) && !atomic_exchange(&s_order_logged, true))
      OLOG_WARNING("email panel: account %lld listed out of date order; sorted",
                   (long long)a->from.account_id);
   if (have == 0 && more)
      OLOG_WARNING("email panel: account %lld: %d pages with nothing to show%s",
                   (long long)a->from.account_id, GMAIL_FETCH_PAGES,
                   a->from.next_date > 0 ? "; stepping past one crowded second" : "");
   a->row_count = have;
   a->more = more;
}

static void list_op(const email_exec_task_ctx_t *t) {
   list_ctx_t *c = (list_ctx_t *)t->ctx;
   list_acct_t *a = &c->acct[t->index];
   if (t->lease_err != EMAIL_ERR_NONE) {
      a->err = t->lease_err;
      return;
   }
   /* IMAP fetches a page; Gmail a page plus the resume second's repeats. */
   const int rows = a->from.is_imap ? c->limit : GMAIL_FETCH_ROWS(c->limit);
   a->rows = calloc((size_t)rows, sizeof(*a->rows));
   if (!a->rows) {
      a->err = EMAIL_ERR_FAILED;
      return;
   }
   if (a->from.is_imap)
      list_imap(c, a, t);
   else
      list_gmail(c, a, t);
}

static json_object *add_account_status(json_object *arr, int64_t id, email_err_t err, int unread) {
   json_object *o = json_object_new_object();
   if (!o)
      return NULL;
   json_object_object_add(o, "account_id", json_object_new_int64(id));
   json_object_object_add(o, "inbox_unread",
                          err == EMAIL_ERR_NONE && unread >= 0 ? json_object_new_int(unread)
                                                               : NULL);
   json_object_object_add(o, "status", json_object_new_string(email_wire_account_status(err)));
   json_object_array_add(arr, o);
   return o;
}

static void add_partial(json_object *arr, int64_t id, email_err_t err) {
   json_object *o = json_object_new_object();
   json_object_object_add(o, "account_id", json_object_new_int64(id));
   json_object_object_add(o, "error_code", json_object_new_string(email_error_name(err)));
   json_object_object_add(o, "error", json_object_new_string(email_wire_error_text(err)));
   json_object_array_add(arr, o);
}

static json_object *list_finish(void *ctx, const int64_t *account_ids, int n) {
   (void)account_ids;
   list_ctx_t *c = (list_ctx_t *)ctx;
   if (n != c->n)
      return email_panel_error_payload(EMAIL_ERR_FAILED);
   for (int i = 0; i < c->n; i++) {
      if (c->acct[i].err == EMAIL_ERR_CURSOR_STALE)
         return email_panel_error_payload(EMAIL_ERR_CURSOR_STALE);
   }

   email_merge_in_t in[EMAIL_MAX_ACCOUNTS];
   for (int i = 0; i < c->n; i++) {
      const list_acct_t *a = &c->acct[i];
      in[i] = (email_merge_in_t){
         .account_id = a->from.account_id,
         .is_imap = a->from.is_imap,
         .err = a->err,
         .from = a->from,
         .rows = a->rows,
         .row_count = a->row_count,
         .more = a->more,
         .next_before_uid = a->next_before,
         .next_uidvalidity = a->next_v,
         .next_folder_hash = a->next_f,
      };
   }
   email_merge_pick_t picks[PANEL_LIMIT_MAX];
   int pick_count = 0;
   email_cursor_t next;
   memset(&next, 0, sizeof(next));
   next.filter = c->filter;
   const bool more = email_merge_page(in, c->n, c->limit, picks, &pick_count, &next);

   json_object *payload = json_object_new_object();
   json_object *rows = json_object_new_array();
   json_object *partial = json_object_new_array();
   if (!payload || !rows || !partial) {
      json_object_put(payload);
      json_object_put(rows);
      json_object_put(partial);
      return NULL;
   }
   for (int i = 0; i < pick_count; i++) {
      const list_acct_t *a = &c->acct[picks[i].account];
      json_object_array_add(rows, email_wire_row(a->from.account_id, &a->rows[picks[i].row],
                                                 !a->from.is_imap));
   }
   json_object_object_add(payload, "rows", rows);
   char *cursor = more ? email_cursor_encode(&next) : NULL;
   json_object_object_add(payload, "cursor", cursor ? json_object_new_string(cursor) : NULL);
   free(cursor);

   for (int i = 0; i < c->missing_count; i++)
      add_partial(partial, c->missing[i], EMAIL_ERR_ACCOUNT_NOT_FOUND);
   for (int i = 0; i < c->n; i++) {
      if (c->acct[i].err != EMAIL_ERR_NONE)
         add_partial(partial, c->acct[i].from.account_id, c->acct[i].err);
   }
   json_object_object_add(payload, "partial", partial);

   if (c->want_counts) {
      json_object *accounts = json_object_new_array();
      for (int i = 0; accounts && i < c->n; i++) {
         const email_cursor_pos_t *f = &c->acct[i].from;
         json_object *o = add_account_status(accounts, f->account_id, c->acct[i].err,
                                             c->acct[i].inbox_unread);
         /* What it can move to, once known (the fetch above may have learned it). */
         bool can_trash, can_archive;
         if (o &&
             email_service_account_caps(f->account_id, !f->is_imap, &can_trash, &can_archive)) {
            json_object_object_add(o, "can_trash", json_object_new_boolean(can_trash));
            json_object_object_add(o, "can_archive", json_object_new_boolean(can_archive));
         }
      }
      json_object_object_add(payload, "accounts", accounts);
   }
   return payload;
}

static void list_free(void *ctx) {
   list_ctx_t *c = (list_ctx_t *)ctx;
   if (!c)
      return;
   for (int i = 0; i < c->n; i++)
      free(c->acct[i].rows);
   free(c);
}

/* The account_ids member: false when it isn't a list of up to 16 distinct ids. */
static bool get_account_ids(json_object *payload, int64_t *ids, int *n, bool *present) {
   json_object *arr;
   *n = 0;
   *present = false;
   if (!payload || !json_object_object_get_ex(payload, "account_ids", &arr))
      return true;
   if (!json_object_is_type(arr, json_type_array))
      return false;
   const size_t len = json_object_array_length(arr);
   if (len == 0 || len > EMAIL_MAX_ACCOUNTS)
      return false;
   for (size_t i = 0; i < len; i++) {
      json_object *v = json_object_array_get_idx(arr, i);
      if (!json_object_is_type(v, json_type_int))
         return false;
      const int64_t id = json_object_get_int64(v);
      if (id < 1)
         return false;
      for (int j = 0; j < *n; j++) {
         if (ids[j] == id)
            return false;
      }
      ids[(*n)++] = id;
   }
   *present = true;
   return true;
}

static void list_or_search(ws_connection_t *conn, json_object *payload, bool search) {
   const char *verb = search ? "email_search" : "email_list";
   char req_buf[EMAIL_EXEC_REQ_MAX + 1];
   const char *req = NULL;
   if (!email_panel_verb_start(conn, verb, payload, req_buf, &req))
      return;

   list_ctx_t *c = calloc(1, sizeof(*c));
   if (!c) {
      email_panel_reply_error(conn, verb, EMAIL_ERR_FAILED, req);
      return;
   }
   c->search = search;

   int64_t limit = 0;
   bool has_folder = false, has_query = false, has_cursor = false, has_ids = false;
   char cursor_b64[EMAIL_CURSOR_B64_MAX + 1];
   int64_t ids[EMAIL_MAX_ACCOUNTS];
   int n_ids = 0;
   bool ok = get_int(payload, "limit", 1, PANEL_LIMIT_MAX, PANEL_LIMIT_DEFAULT, &limit) &&
             get_bool(payload, "unread_only", false, &c->unread_only) &&
             get_text(payload, "cursor", EMAIL_CURSOR_B64_MAX, cursor_b64, sizeof(cursor_b64),
                      &has_cursor) &&
             get_account_ids(payload, ids, &n_ids, &has_ids);
   if (ok && search) {
      /* Search looks through all mail where the backend has it (Gmail: in:all). */
      ok = get_text(payload, "query", EMAIL_SEARCH_TEXT_MAX, c->query, sizeof(c->query),
                    &has_query) &&
           has_query;
      snprintf(c->folder, sizeof(c->folder), "all");
   } else if (ok) {
      ok = get_text(payload, "folder", PANEL_FOLDER_MAX, c->folder, sizeof(c->folder), &has_folder);
      if (ok && !has_folder)
         snprintf(c->folder, sizeof(c->folder), "inbox");
      ok = ok && email_service_validate_folder_name(c->folder);
   }
   if (!ok) {
      free(c);
      email_panel_reply_error(conn, verb, EMAIL_ERR_INVALID_REQUEST, req);
      return;
   }
   c->limit = (int)limit;

   panel_acct_t accts[EMAIL_MAX_ACCOUNTS];
   const int n_accts = email_panel_load_accounts(conn->auth_user_id, accts);
   if (n_accts == 0) {
      free(c);
      email_panel_reply_error(conn, verb, EMAIL_ERR_NO_ACCOUNT, req);
      return;
   }
   /* The folder as it's used (omitted = inbox), so the two spellings share cursors. */
   c->filter = email_cursor_filter_hash(verb, has_ids ? ids : NULL, n_ids, c->unread_only,
                                        c->folder, c->query);

   if (has_cursor) {
      email_cursor_t cur;
      bool stale = !email_cursor_decode(cursor_b64, &cur) || cur.filter != c->filter ||
                   cur.count == 0;
      for (int i = 0; !stale && i < cur.count; i++) {
         const panel_acct_t *a = find_acct(accts, n_accts, cur.pos[i].account_id);
         bool asked = !has_ids;
         for (int j = 0; !asked && j < n_ids; j++)
            asked = ids[j] == cur.pos[i].account_id;
         stale = !a || !asked || a->is_imap != cur.pos[i].is_imap;
         if (!stale)
            c->acct[c->n++].from = cur.pos[i];
      }
      if (stale) {
         free(c);
         email_panel_reply_error(conn, verb, EMAIL_ERR_CURSOR_STALE, req);
         return;
      }
   } else if (has_ids) {
      for (int i = 0; i < n_ids; i++) {
         const panel_acct_t *a = find_acct(accts, n_accts, ids[i]);
         if (!a) {
            c->missing[c->missing_count++] = ids[i];
            continue;
         }
         c->acct[c->n].from.account_id = a->id;
         c->acct[c->n++].from.is_imap = a->is_imap;
      }
   } else {
      for (int i = 0; i < n_accts; i++) {
         c->acct[c->n].from.account_id = accts[i].id;
         c->acct[c->n++].from.is_imap = accts[i].is_imap;
      }
   }
   c->want_counts = !search && !has_cursor && strcasecmp(c->folder, "inbox") == 0;

   bool have_session = false;
   const uint32_t session_id = email_panel_session_id_of(conn, &have_session);
   if (c->n == 0 || !have_session) {
      /* Nothing to fetch (every id asked for is gone): answer now. */
      json_object *p = have_session ? list_finish(c, NULL, 0)
                                    : email_panel_error_payload(EMAIL_ERR_FAILED);
      list_free(c);
      email_panel_reply(conn, verb, p, req);
      return;
   }

   int64_t task_ids[EMAIL_MAX_ACCOUNTS];
   bool is_imap[EMAIL_MAX_ACCOUNTS];
   for (int i = 0; i < c->n; i++) {
      task_ids[i] = c->acct[i].from.account_id;
      is_imap[i] = c->acct[i].from.is_imap;
      c->acct[i].inbox_unread = -1;
   }
   const email_exec_request_t r = {
      .session_id = session_id,
      .user_id = conn->auth_user_id,
      .slot = EMAIL_EXEC_SLOT_LIST,
      .verb = verb,
      .req = req,
      .task_count = c->n,
      .account_ids = task_ids,
      .is_imap = is_imap,
      .op = list_op,
      .finish = list_finish,
      .free_ctx = list_free,
      .ctx = c,
   };
   email_panel_submit(conn, &r);
}

void handle_email_list(ws_connection_t *conn, json_object *payload) {
   list_or_search(conn, payload, false);
}

void handle_email_search(ws_connection_t *conn, json_object *payload) {
   list_or_search(conn, payload, true);
}

/* =============================================================================
 * email_read
 * ============================================================================= */

typedef struct {
   char message_id[PANEL_MSG_ID_MAX + 1];
   email_mark_t mark;
   int rc;
   email_err_t err;
   email_message_t msg;
} read_ctx_t;

static void read_op(const email_exec_task_ctx_t *t) {
   read_ctx_t *c = (read_ctx_t *)t->ctx;
   c->rc = EMAIL_RC_FAILURE;
   if (t->lease_err != EMAIL_ERR_NONE) {
      c->err = t->lease_err;
      return;
   }
   const email_read_opts_t opts = {
      .fetch_bytes = EMAIL_READ_FETCH_PANEL,
      .max_text_chars = EMAIL_PANEL_TEXT_MAX,
      .max_html_bytes = EMAIL_READ_HTML_PANEL,
      .want_html = true,
      .cancel = t->cancel,
      .mark = c->mark,
   };
   c->rc = email_service_read(t->user_id, NULL, c->message_id, &opts, &c->msg, t->target, &c->err);
}

static json_object *read_finish(void *ctx, const int64_t *account_ids, int n) {
   read_ctx_t *c = (read_ctx_t *)ctx;
   if (c->rc != EMAIL_RC_OK || n != 1)
      return email_panel_error_payload(c->err == EMAIL_ERR_NONE ? EMAIL_ERR_FAILED : c->err);
   /* The state the read left, when it knows; otherwise what the mark asked for. */
   const bool unread = c->msg.unread_known
                           ? c->msg.unread_after
                           : (c->mark == EMAIL_MARK_READ ? false : c->msg.unread_before);
   json_object *p = email_wire_read_payload(account_ids[0], &c->msg, unread, EMAIL_PANEL_FRAME_MAX);
   return p ? p : email_panel_error_payload(EMAIL_ERR_FAILED);
}

static void read_free(void *ctx) {
   read_ctx_t *c = (read_ctx_t *)ctx;
   if (!c)
      return;
   email_message_free(&c->msg);
   free(c);
}

/* The account_id member: the user's enabled account, else NULL. */
const panel_acct_t *email_panel_get_account(json_object *payload,
                                            const panel_acct_t *accts,
                                            int n_accts,
                                            bool *valid) {
   int64_t id = 0;
   json_object *v;
   *valid = payload && json_object_object_get_ex(payload, "account_id", &v) &&
            get_int(payload, "account_id", 1, INT64_MAX, 0, &id) && id > 0;
   return *valid ? find_acct(accts, n_accts, id) : NULL;
}

void handle_email_read(ws_connection_t *conn, json_object *payload) {
   const char *verb = "email_read";
   char req_buf[EMAIL_EXEC_REQ_MAX + 1];
   const char *req = NULL;
   if (!email_panel_verb_start(conn, verb, payload, req_buf, &req))
      return;

   read_ctx_t *c = calloc(1, sizeof(*c));
   if (!c) {
      email_panel_reply_error(conn, verb, EMAIL_ERR_FAILED, req);
      return;
   }
   bool has_id = false, mark_read = true;
   if (!get_text(payload, "message_id", PANEL_MSG_ID_MAX, c->message_id, sizeof(c->message_id),
                 &has_id) ||
       !has_id || !get_bool(payload, "mark_read", true, &mark_read)) {
      free(c);
      email_panel_reply_error(conn, verb, EMAIL_ERR_INVALID_REQUEST, req);
      return;
   }
   c->mark = mark_read ? EMAIL_MARK_READ : EMAIL_MARK_KEEP;

   panel_acct_t accts[EMAIL_MAX_ACCOUNTS];
   const int n_accts = email_panel_load_accounts(conn->auth_user_id, accts);
   bool valid = false;
   const panel_acct_t *a = email_panel_get_account(payload, accts, n_accts, &valid);
   bool have_session = false;
   const uint32_t session_id = email_panel_session_id_of(conn, &have_session);
   if (!valid || !a || !have_session) {
      free(c);
      email_panel_reply_error(conn, verb,
                              !valid ? EMAIL_ERR_INVALID_REQUEST
                                     : (!a ? EMAIL_ERR_ACCOUNT_NOT_FOUND : EMAIL_ERR_FAILED),
                              req);
      return;
   }
   if (!email_panel_msg_id_ok(c->message_id, a->is_imap)) {
      free(c);
      email_panel_reply_error(conn, verb, EMAIL_ERR_INVALID_REQUEST, req);
      return;
   }
   const int64_t id = a->id;
   const bool is_imap = a->is_imap;
   const email_exec_request_t r = {
      .session_id = session_id,
      .user_id = conn->auth_user_id,
      .slot = EMAIL_EXEC_SLOT_READ,
      .verb = verb,
      .req = req,
      .task_count = 1,
      .account_ids = &id,
      .is_imap = &is_imap,
      .op = read_op,
      .finish = read_finish,
      .free_ctx = read_free,
      .ctx = c,
   };
   email_panel_submit(conn, &r);
}

/* =============================================================================
 * email_set_flags
 * ============================================================================= */

typedef struct {
   int n;
   char ids[EMAIL_FLAGS_MAX_IDS][PANEL_MSG_ID_MAX + 1];
   bool unread;
   int rc;
   email_err_t err;
   email_flag_result_t results[EMAIL_FLAGS_MAX_IDS];
} flags_ctx_t;

static void flags_op(const email_exec_task_ctx_t *t) {
   flags_ctx_t *c = (flags_ctx_t *)t->ctx;
   c->rc = EMAIL_RC_FAILURE;
   if (t->lease_err != EMAIL_ERR_NONE) {
      c->err = t->lease_err;
      return;
   }
   const char *ptrs[EMAIL_FLAGS_MAX_IDS];
   for (int i = 0; i < c->n; i++)
      ptrs[i] = c->ids[i];
   c->rc = email_service_set_flags(t->user_id, t->target, ptrs, c->n, c->unread, c->results,
                                   &c->err);
}

static json_object *flags_finish(void *ctx, const int64_t *account_ids, int n) {
   (void)account_ids;
   (void)n;
   flags_ctx_t *c = (flags_ctx_t *)ctx;
   if (c->rc != EMAIL_RC_OK)
      return email_panel_error_payload(c->err == EMAIL_ERR_NONE ? EMAIL_ERR_FAILED : c->err);
   json_object *p = json_object_new_object();
   json_object *updated = json_object_new_array();
   json_object *failed = json_object_new_array();
   if (!p || !updated || !failed) {
      json_object_put(p);
      json_object_put(updated);
      json_object_put(failed);
      return NULL;
   }
   for (int i = 0; i < c->n; i++) {
      if (c->results[i].updated) {
         json_object_array_add(updated, json_object_new_string(c->ids[i]));
         continue;
      }
      const email_err_t e = c->results[i].err == EMAIL_ERR_NONE ? EMAIL_ERR_FAILED
                                                                : c->results[i].err;
      json_object *f = json_object_new_object();
      json_object_object_add(f, "message_id", json_object_new_string(c->ids[i]));
      json_object_object_add(f, "error_code", json_object_new_string(email_error_name(e)));
      json_object_object_add(f, "error", json_object_new_string(email_wire_error_text(e)));
      json_object_array_add(failed, f);
   }
   json_object_object_add(p, "updated", updated);
   json_object_object_add(p, "failed", failed);
   return p;
}

static void flags_free(void *ctx) {
   free(ctx);
}

void handle_email_set_flags(ws_connection_t *conn, json_object *payload) {
   const char *verb = "email_set_flags";
   char req_buf[EMAIL_EXEC_REQ_MAX + 1];
   const char *req = NULL;
   if (!email_panel_verb_start(conn, verb, payload, req_buf, &req))
      return;

   flags_ctx_t *c = calloc(1, sizeof(*c));
   if (!c) {
      email_panel_reply_error(conn, verb, EMAIL_ERR_FAILED, req);
      return;
   }
   json_object *arr = NULL, *unread_obj = NULL;
   bool ok = payload && json_object_object_get_ex(payload, "message_ids", &arr) &&
             json_object_is_type(arr, json_type_array) && json_object_array_length(arr) >= 1 &&
             json_object_array_length(arr) <= EMAIL_FLAGS_MAX_IDS &&
             json_object_object_get_ex(payload, "unread", &unread_obj) &&
             json_object_is_type(unread_obj, json_type_boolean);
   for (size_t i = 0; ok && i < json_object_array_length(arr); i++) {
      json_object *v = json_object_array_get_idx(arr, i);
      ok = email_panel_text_ok(v, PANEL_MSG_ID_MAX);
      if (ok) {
         memcpy(c->ids[c->n], json_object_get_string(v), (size_t)json_object_get_string_len(v) + 1);
         c->n++;
      }
   }
   if (!ok) {
      free(c);
      email_panel_reply_error(conn, verb, EMAIL_ERR_INVALID_REQUEST, req);
      return;
   }
   c->unread = json_object_get_boolean(unread_obj);

   panel_acct_t accts[EMAIL_MAX_ACCOUNTS];
   const int n_accts = email_panel_load_accounts(conn->auth_user_id, accts);
   bool valid = false;
   const panel_acct_t *a = email_panel_get_account(payload, accts, n_accts, &valid);
   bool have_session = false;
   const uint32_t session_id = email_panel_session_id_of(conn, &have_session);
   if (!valid || !a || !have_session) {
      free(c);
      email_panel_reply_error(conn, verb,
                              !valid ? EMAIL_ERR_INVALID_REQUEST
                                     : (!a ? EMAIL_ERR_ACCOUNT_NOT_FOUND : EMAIL_ERR_FAILED),
                              req);
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
      .slot = EMAIL_EXEC_SLOT_FLAGS,
      .verb = verb,
      .req = req,
      .task_count = 1,
      .account_ids = &id,
      .is_imap = &is_imap,
      .op = flags_op,
      .finish = flags_finish,
      .free_ctx = flags_free,
      .ctx = c,
   };
   email_panel_submit(conn, &r);
}

/* =============================================================================
 * email_unread_counts
 * ============================================================================= */

typedef struct {
   int n;
   int counts[EMAIL_MAX_ACCOUNTS];
   email_err_t errs[EMAIL_MAX_ACCOUNTS];
} counts_ctx_t;

static void counts_op(const email_exec_task_ctx_t *t) {
   counts_ctx_t *c = (counts_ctx_t *)t->ctx;
   c->counts[t->index] = -1;
   if (t->lease_err != EMAIL_ERR_NONE) {
      c->errs[t->index] = t->lease_err;
      return;
   }
   email_err_t err = EMAIL_ERR_FAILED;
   if (email_service_unread_count(t->user_id, t->target, &c->counts[t->index], &err) != EMAIL_RC_OK)
      c->errs[t->index] = err == EMAIL_ERR_NONE ? EMAIL_ERR_FAILED : err;
}

static json_object *counts_finish(void *ctx, const int64_t *account_ids, int n) {
   counts_ctx_t *c = (counts_ctx_t *)ctx;
   json_object *p = json_object_new_object();
   json_object *accounts = json_object_new_array();
   if (!p || !accounts) {
      json_object_put(p);
      json_object_put(accounts);
      return NULL;
   }
   for (int i = 0; i < n && i < c->n; i++)
      add_account_status(accounts, account_ids[i], c->errs[i], c->counts[i]);
   json_object_object_add(p, "accounts", accounts);
   return p;
}

static void counts_free(void *ctx) {
   free(ctx);
}

void handle_email_unread_counts(ws_connection_t *conn, json_object *payload) {
   const char *verb = "email_unread_counts";
   char req_buf[EMAIL_EXEC_REQ_MAX + 1];
   const char *req = NULL;
   if (!email_panel_verb_start(conn, verb, payload, req_buf, &req))
      return;

   panel_acct_t accts[EMAIL_MAX_ACCOUNTS];
   const int n_accts = email_panel_load_accounts(conn->auth_user_id, accts);
   bool have_session = false;
   const uint32_t session_id = email_panel_session_id_of(conn, &have_session);
   if (n_accts == 0 || !have_session) {
      json_object *p = json_object_new_object();
      if (p)
         json_object_object_add(p, "accounts", json_object_new_array());
      email_panel_reply(conn, verb,
                        have_session
                            ? p
                            : (json_object_put(p), email_panel_error_payload(EMAIL_ERR_FAILED)),
                        req);
      return;
   }
   counts_ctx_t *c = calloc(1, sizeof(*c));
   if (!c) {
      email_panel_reply_error(conn, verb, EMAIL_ERR_FAILED, req);
      return;
   }
   c->n = n_accts;
   int64_t ids[EMAIL_MAX_ACCOUNTS];
   bool is_imap[EMAIL_MAX_ACCOUNTS];
   for (int i = 0; i < n_accts; i++) {
      ids[i] = accts[i].id;
      is_imap[i] = accts[i].is_imap;
   }
   const email_exec_request_t r = {
      .session_id = session_id,
      .user_id = conn->auth_user_id,
      .slot = EMAIL_EXEC_SLOT_COUNTS,
      .verb = verb,
      .req = req,
      .task_count = n_accts,
      .account_ids = ids,
      .is_imap = is_imap,
      .op = counts_op,
      .finish = counts_finish,
      .free_ctx = counts_free,
      .ctx = c,
   };
   email_panel_submit(conn, &r);
}
