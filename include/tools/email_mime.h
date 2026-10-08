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
 * Reading a message: one policy over the parts of a message, whichever
 * backend fetched it.  IMAP hands over the raw RFC 822 bytes (parsed here
 * with GMime); Gmail hands over the parts its API already split (gmail_parts.c
 * walks the JSON into email_mime_part_t).  The policy picks the body text and
 * HTML from the message's structure, decodes them to UTF-8 within their caps,
 * lists the attachments and fills an email_message_t.  The only file that
 * sees GMime.
 */

#ifndef EMAIL_MIME_H
#define EMAIL_MIME_H

#include <stdbool.h>
#include <stddef.h>

#include "tools/email_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Limits on what one message may make us do.  Past one, the rest of the
 * message is treated as cut (reported as truncated, never refused). */
#define EMAIL_MIME_MAX_PARTS 256                    /* nodes (multiparts and leaves) walked */
#define EMAIL_MIME_MAX_DEPTH 16                     /* multipart nesting */
#define EMAIL_MIME_PRESCAN_BOUNDARIES 1024          /* "--" lines before GMime sees the bytes */
#define EMAIL_MIME_PRESCAN_HEADERS 5000             /* header lines, all header blocks together */
#define EMAIL_MIME_PRESCAN_HEADER_BYTES (64 * 1024) /* one header, folded lines included */
#define EMAIL_MIME_PRESCAN_LINE (1024 * 1024)       /* longest line */
/* Address headers (To, Cc, From, ...) together: GMime builds every address
 * in them, at a cost that grows faster than their number. */
#define EMAIL_MIME_PRESCAN_ADDRESS_BYTES (256 * 1024)
/* Entries without an '@' in address headers, all together: GMime's parse of
 * them costs time quadratic in their number (a header folded over 13,000 of
 * them takes it 1.6 s).  Real lists have few: a name with a comma in it is
 * one ("Doe, John" quoted, or a sloppy mailer's unquoted Doe, John <j@x>),
 * and so is a group's colon (undisclosed-recipients:;). */
#define EMAIL_MIME_PRESCAN_BARE_ADDRESSES 1000
/* Headers of one address type (To, Cc, ...) in one header block: GMime parses
 * them all again each time another arrives.  Real mail has one of each. */
#define EMAIL_MIME_PRESCAN_ADDRESS_REPEATS 4
/* An address header longer than this is cut (at an address boundary) before
 * it is parsed: building a list costs per address, and only 32 are kept. */
#define EMAIL_MIME_ADDR_VALUE_MAX (64 * 1024)

/* How a part's bytes are encoded on the wire. */
typedef enum {
   EMAIL_MIME_ENC_NONE = 0, /* 7bit, 8bit, binary, or already decoded (Gmail) */
   EMAIL_MIME_ENC_BASE64,
   EMAIL_MIME_ENC_QP,
   EMAIL_MIME_ENC_UUENCODE,
} email_mime_enc_t;

/* One node of a message, as a backend found it, in document order (a
 * multipart before its children).  A multipart is a node too (multipart set,
 * type "multipart/<subtype>"); its part_id is its IMAP section number, "" for
 * the message's own top multipart.  Its children are the nodes that follow
 * whose part_id is "<its id>.<n>" ("<n>" for the top). */
typedef struct {
   char part_id[32];   /* IMAP section number */
   bool multipart;     /* a container: only part_id and type are meaningful */
   char type[96];      /* lowercased type/subtype */
   char charset[64];   /* "" when not given */
   char filename[256]; /* decoded, not yet sanitized; "" when none */
   char content_id[256];
   bool attachment;     /* Content-Disposition: attachment */
   bool inline_disp;    /* Content-Disposition: inline */
   bool in_related;     /* inside multipart/related */
   bool in_alternative; /* inside multipart/alternative */
   size_t size;         /* decoded size, or an estimate */
   /* The content, for the parts the policy may read (text).  Not owned. */
   const char *data;
   size_t data_len;
   email_mime_enc_t encoding;
   bool data_cut;    /* the content runs into the end of a cut fetch */
   bool data_absent; /* the backend has it elsewhere (a Gmail attachmentId) */
} email_mime_part_t;

/**
 * @brief Fetches the content of part @p index, which the backend holds
 *        elsewhere (data_absent; a Gmail attachmentId)
 *
 * Called by email_mime_apply only for a part it is about to read, in reading
 * order, so nothing past the text cap is fetched.  The bytes must stay alive
 * until email_mime_apply returns.
 *
 * @param cut_out Set when the bytes are not the whole part
 * @return false when the part couldn't be fetched (it reads as cut)
 */
typedef bool (*email_mime_fetch_fn)(void *ctx,
                                    int index,
                                    const char **data_out,
                                    size_t *len_out,
                                    bool *cut_out);

/**
 * @brief Fill @p out's body, HTML and attachments from @p parts
 *
 * The body follows the message's structure: of a multipart/alternative, one
 * branch (the text/plain one for the text, else the HTML one as text); of a
 * mixed or related multipart, every inline text part in order (an HTML part
 * turned into text).  The HTML (with want_html) is the HTML branch's part.
 * Text is decoded to UTF-8 within @p opts' caps (a streaming decode: a large
 * part is never decoded whole).  Every leaf not read, and not just another
 * form of the body, is listed as an attachment.  Headers are the caller's.
 *
 * @param parts_cut true when the backend stopped before the end of the message
 * @param fetch     Fetches a data_absent part when it is read (NULL: such a part reads as cut)
 * @return 0, or 1 (out of memory) with no heap left in @p out's body fields
 */
int email_mime_apply(const email_mime_part_t *parts,
                     int count,
                     bool parts_cut,
                     const email_read_opts_t *opts,
                     email_mime_fetch_fn fetch,
                     void *fetch_ctx,
                     email_message_t *out);

/**
 * @brief Read a raw RFC 822 message (IMAP) into @p out
 *
 * Bounds the bytes first (email_mime_prescan), parses them with GMime (no copy
 * of @p raw, which must stay alive for the call), fills the headers, then
 * email_mime_apply.  With opts->headers_only, only the headers.
 *
 * @param raw_cut true when the fetch stopped at its byte limit
 * @return 0, or 1 with no heap left in @p out
 */
int email_mime_parse_raw(char *raw,
                         size_t raw_len,
                         bool raw_cut,
                         const email_read_opts_t *opts,
                         email_message_t *out);

/**
 * @brief How much of @p raw GMime may see: the length up to the first line
 *        past a limit (EMAIL_MIME_PRESCAN_*), or @p raw_len
 * @param cut_out true when the length was cut
 */
size_t email_mime_prescan(const char *raw, size_t raw_len, bool *cut_out);

/**
 * @brief Parse an address header ("Name <a@b>, c@d", RFC 2047 words) into a heap list
 * @param list_out  NULL when there are none; caller frees
 * @param count_out Addresses kept (at most @p cap)
 * @param total_out Addresses in the header
 * @return 0, or 1 when out of memory
 */
int email_mime_addr_list(const char *value,
                         int cap,
                         email_addr_t **list_out,
                         int *count_out,
                         int *total_out);

/** The first address of @p value (From, Reply-To) into @p out; false when none. */
bool email_mime_addr_first(const char *value, email_addr_t *out);

/**
 * @brief Fill a part's charset, disposition and filename from its raw
 *        Content-Type and Content-Disposition values (either may be NULL)
 *
 * The filename (RFC 2231 or 2047) is taken only when @p p has none yet.
 */
void email_mime_part_headers(const char *content_type,
                             const char *disposition,
                             email_mime_part_t *p);

/**
 * @brief The IMAP section number of child @p index (1-based) of node @p parent
 *        ("" for the top multipart): "<parent>.<index>", or "<index>"
 * @return false when it doesn't fit (it would then name another part)
 */
bool email_mime_child_id(const char *parent, int index, char *out, size_t size);

/** Decode a header value's RFC 2047 words into display text (email_display rules). */
void email_mime_header_text(const char *value, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_MIME_H */
