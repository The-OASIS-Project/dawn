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
 *
 * Shared email types — used by both IMAP (email_client) and Gmail API (gmail_client).
 */

#ifndef EMAIL_TYPES_H
#define EMAIL_TYPES_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Maximum emails to fetch in a single request (bounds stack usage).
 * handle_recent/handle_search hold an email_summary_t[50] (~1.66KB each ≈ 83KB)
 * on the stack.  The binding limit is NOT the 8MB default stack but the 512KB
 * parallel tool-execution thread (llm_tools.c) the email tool runs on when the
 * LLM batches tool calls — 83KB is ~16% of it, comfortable but the number to
 * measure against if this struct grows.  If it grows materially, move those
 * arrays to a calloc'd scratch (as email_digest.c does across accounts) rather
 * than raising this cap. */
#define EMAIL_MAX_FETCH_RESULTS 50

/* Buffer size for an opaque paging cursor (Gmail nextPageToken or IMAP "u<uid>.<v>");
 * matches email_search_params_t.page_token. */
#define EMAIL_PAGE_TOKEN_LEN 256

/* Rows `recent` returns when the caller gives no count and the account has no
 * max_recent override.  The email_accounts base schema spells this default as a
 * literal (max_recent INTEGER DEFAULT 10); every insert binds an explicit value,
 * so the schema literal only matters for hand-inserted rows. */
#define EMAIL_MAX_RECENT_DEFAULT 10

/* Per-account digest depth: how many of an inbox's newest messages the daily
 * digest may scan, reached by paging EMAIL_MAX_FETCH_RESULTS at a time.  The
 * default equals the old single-fetch depth, so existing digests are unchanged.
 * NOTE: the auth layer cannot include tools headers, so the email_accounts
 * schema default + v87 migration mirror the default as EMAIL_DEFAULT_DIGEST_DEPTH
 * in include/auth/auth_db_internal.h — keep the two in sync. */
#define EMAIL_DIGEST_DEPTH_DEFAULT 50
#define EMAIL_DIGEST_DEPTH_MAX 200
/* Page ceiling per account per digest: bounds the loop even if a backend keeps
 * returning short pages with a continuation token. */
#define EMAIL_DIGEST_MAX_PAGES (EMAIL_DIGEST_DEPTH_MAX / EMAIL_MAX_FETCH_RESULTS)

/* Inbound (read) body cap — default fallback for both backends when an account
 * sets no max_body_chars override, and the upper bound the WebUI accepts for the
 * per-account override.  Read bodies are heap-allocated, so this can be large;
 * only excessive messages truncate.  Shared here (rather than in the
 * service-layer header) so both IMAP and Gmail backends reference one owner.
 * The outbound (send) cap lives in email_service.h — it is bounded by the fixed
 * email_draft_t.body[] buffer and is a service-layer concern.
 * NOTE: the auth layer cannot include tools headers, so the email_accounts
 * schema default + v55 migration mirror this value as EMAIL_DEFAULT_BODY_CHARS
 * in include/auth/auth_db_internal.h — keep the two in sync. */
#define EMAIL_MAX_READ_BODY_LEN 50000

/* Lower bound the WebUI accepts for a per-account max_body_chars override. */
#define EMAIL_MIN_READ_BODY_LEN 500

/* Whether the user has replied to a message.  Tri-state so a skipped, failed, or
 * over-budget thread lookup is reported as UNKNOWN, never as "not replied". */
typedef enum {
   EMAIL_REPLIED_UNKNOWN = 0, /* not determined (default; lookup skipped/failed) */
   EMAIL_REPLIED_NO,          /* no later reply found in the thread */
   EMAIL_REPLIED_YES,         /* a SENT message exists later in the thread */
} email_reply_state_t;

/* Gmail inbox category (from CATEGORY_* labels).  PRIMARY is the default when no
 * category label is present (or on IMAP, which has no categories). */
typedef enum {
   EMAIL_CAT_PRIMARY = 0,
   EMAIL_CAT_SOCIAL,
   EMAIL_CAT_PROMOTIONS,
   EMAIL_CAT_UPDATES,
   EMAIL_CAT_FORUMS,
} email_category_t;

typedef struct {
   uint32_t uid;
   char message_id[192];   /* Gmail hex ID or IMAP folder:uid composite */
   char thread_id[192];    /* Gmail threadId (for reply/dedup grouping); empty on IMAP */
   char account_name[128]; /* configured account display name (email_account_t.name) — may be a
                            * friendly label like "Gmail" that does not identify the inbox */
   char account_addr[128]; /* the account's address (email_account_t.username) — the unambiguous
                            * inbox identifier; use this to disambiguate across accounts */
   char from_name[64];
   char from_addr[256];
   char subject[256];
   char date_str[32];
   time_t date;
   char preview[512];
   bool unread;                 /* Gmail UNREAD label / IMAP \Seen-absent — populated on both
                                 * backends (IMAP parses FLAGS at fetch time). */
   bool important;              /* Gmail IMPORTANT label */
   bool starred;                /* Gmail STARRED label */
   bool from_me;                /* Gmail SENT label — the user's own message */
   email_category_t category;   /* Gmail inbox category; PRIMARY otherwise */
   email_reply_state_t replied; /* Gmail: filled by the digest reply-enrichment pass (in:sent
                                 * search). IMAP: filled at fetch time from the \Answered flag. */
} email_summary_t;

/* Read-result list caps.  The lists are heap arrays sized to what a message
 * has, up to these; the *_total / *_truncated fields say when more existed. */
#define EMAIL_MAX_ADDRS 32
#define EMAIL_MAX_ATTACHMENTS 16

/* Read fetch sizes: what the LLM tool reads of a message, and what the WebUI
 * mail panel reads (enough for an HTML newsletter with its inline images). */
#define EMAIL_READ_FETCH_TOOL (512 * 1024)
#define EMAIL_READ_FETCH_PANEL (2 * 1024 * 1024)
/* The panel's HTML cap: the browser parses it on its main thread. */
#define EMAIL_READ_HTML_PANEL (1024 * 1024)

/* Why an email operation failed, for messages and the wire (email_error_name). */
typedef enum {
   EMAIL_ERR_NONE = 0,
   EMAIL_ERR_FAILED,            /* unclassified */
   EMAIL_ERR_AUTH_FAILED,       /* the server refused the login or token */
   EMAIL_ERR_AUTH_REVOKED,      /* the OAuth grant was revoked: reconnect the account */
   EMAIL_ERR_UNREACHABLE,       /* resolve, connect or TLS failed */
   EMAIL_ERR_TIMEOUT,           /* the server didn't answer in time */
   EMAIL_ERR_RATE_LIMITED,      /* the provider asked us to slow down */
   EMAIL_ERR_NOT_FOUND,         /* no such message */
   EMAIL_ERR_READ_ONLY,         /* the account is read-only in DAWN */
   EMAIL_ERR_FOLDER_MISSING,    /* no folder for the role (archive) */
   EMAIL_ERR_NO_TRASH,          /* no Trash folder */
   EMAIL_ERR_NO_ACCOUNT,        /* the user has no enabled email account */
   EMAIL_ERR_ACCOUNT_NOT_FOUND, /* no enabled account by that name */
   EMAIL_ERR_CANCELLED,         /* stopped by the caller (email_read_opts_t.cancel) */
} email_err_t;

/** The wire name of @p err ("AUTH_FAILED", ...; "" for NONE).  email_transfer.c */
const char *email_error_name(email_err_t err);

/* How a message is read: how much to fetch and what to produce. */
typedef struct {
   size_t fetch_bytes;        /* IMAP: raw bytes fetched (EMAIL_READ_FETCH_*) */
   int max_text_chars;        /* body_text cap, in bytes */
   size_t max_html_bytes;     /* body_html cap (only with want_html) */
   bool want_html;            /* produce body_html (never for the LLM) */
   bool headers_only;         /* no body is fetched: From and Subject are filled (Date on
                               * Gmail); nothing else is promised */
   const atomic_bool *cancel; /* set it to stop the read's transfers (may be NULL) */
} email_read_opts_t;

typedef struct {
   char name[64];
   char addr[256];
} email_addr_t;

/* One attachment of a read message.  The text is the sender's, display-
 * sanitized (email_display.h). */
typedef struct {
   char part_id[32];     /* IMAP section number ("2", "2.1"), on both backends */
   char filename[128];   /* "" when the part has none */
   char mime[96];        /* lowercased type/subtype */
   char content_id[128]; /* without <>; "" when none or not a plain id */
   size_t size;          /* decoded bytes (an estimate for base64 and QP) */
   bool is_inline;       /* shown in the body (inline disposition, or cid in related) */
} email_attachment_t;

typedef struct {
   uint32_t uid;
   char message_id[192]; /* Gmail hex ID or IMAP folder:uid composite */
   char thread_id[192];  /* Gmail threadId; "" on IMAP */
   char from_name[64];
   char from_addr[256];
   char subject[256];
   char date_str[32];     /* the Date header as sent */
   time_t internal_date;  /* server receive time (Gmail internalDate); 0 when unknown */
   bool unread_before;    /* unread when it was read (Gmail); false when unknown */
   email_addr_t *to_list; /* heap; to_count kept of to_total */
   int to_count;
   int to_total;
   email_addr_t *cc_list; /* heap */
   int cc_count;
   int cc_total;
   email_addr_t reply_to; /* addr "" when the message has none */
   char *body;            /* body text, heap; caller frees via email_message_free() */
   int body_len;          /* MUST equal strlen(body); callers size read buffers from it */
   char *body_html;       /* heap, valid UTF-8 but NOT HTML-sanitized; only with want_html */
   size_t body_html_len;
   bool text_truncated; /* body is not the whole text (cap, cut fetch or limits) */
   bool html_truncated;
   email_attachment_t *attachments; /* heap */
   int attachment_count;
   bool attachments_truncated; /* more attachments than listed (cap, cut fetch, limits) */
} email_message_t;

typedef struct {
   char from[128];
   char subject[128];
   char text[128];
   char since[16]; /* YYYY-MM-DD, validated via strptime/strftime */
   char before[16];
   bool unread_only;                      /* Only match UNSEEN messages */
   char folder[256];                      /* Folder/label or normalized Gmail query fragment */
   char page_token[EMAIL_PAGE_TOKEN_LEN]; /* Paging cursor, Gmail or IMAP (empty = first page) */
} email_search_params_t;

/**
 * @brief Free the heap fields of an email_message_t and null them.
 * A read that fails leaves none, so calling this after a failure is safe and
 * not needed.
 */
void email_message_free(email_message_t *msg);

#endif /* EMAIL_TYPES_H */
