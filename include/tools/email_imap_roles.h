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
 * What an IMAP account can do and where its special folders are: the server's
 * CAPABILITY, its NAMESPACE, and the folders its LIST marks as Trash or
 * Archive, with a name table for servers that don't mark them.  Only the
 * user's own folders count: anything under another user's or a shared
 * namespace is never a target.  Pure parsing; the network side is in
 * email_imap_move.c.
 */

#ifndef EMAIL_IMAP_ROLES_H
#define EMAIL_IMAP_ROLES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Longest folder name a role can hold, with its NUL. */
#define EMAIL_IMAP_ROLE_FOLDER_MAX 256
/** Longest namespace prefix kept, with its NUL. */
#define EMAIL_IMAP_NS_PREFIX_MAX 128
/** Other users' and shared namespaces kept (more are ignored). */
#define EMAIL_IMAP_NS_OTHER_MAX 8

/** An account's capabilities and special folders. */
typedef struct {
   bool move;        /**< RFC 6851 MOVE: one atomic move, no \Deleted left behind */
   bool uidplus;     /**< RFC 4315 UIDPLUS: UID EXPUNGE of just the moved message */
   bool special_use; /**< RFC 6154 SPECIAL-USE: LIST (SPECIAL-USE) lists the marked folders */
   bool namespace_;  /**< RFC 2342 NAMESPACE */
   bool gmail;       /**< X-GM-EXT-1: Gmail, whose \All (All Mail) is where archive goes */
   /** The folder trash moves to ("" = none: trash is refused, never an expunge). */
   char trash[EMAIL_IMAP_ROLE_FOLDER_MAX];
   /** The folder archive moves to ("" = none). */
   char archive[EMAIL_IMAP_ROLE_FOLDER_MAX];
} email_imap_roles_t;

/** The user's own namespace and the ones that aren't theirs (RFC 2342). */
typedef struct {
   /** e.g. "" or "INBOX." (a prefix sent without its trailing delimiter gets it) */
   char personal[EMAIL_IMAP_NS_PREFIX_MAX];
   char delim; /**< its hierarchy delimiter ('\0' = none) */
   int n_other;
   /** Prefixes of other users' and shared namespaces (each non-empty). */
   char other[EMAIL_IMAP_NS_OTHER_MAX][EMAIL_IMAP_NS_PREFIX_MAX];
} email_imap_ns_t;

/**
 * @brief Read MOVE, UIDPLUS, SPECIAL-USE, NAMESPACE and X-GM-EXT-1 from a
 *        CAPABILITY response
 * @param response The server's reply (one or more "* CAPABILITY ..." lines)
 * @param out      Its capability fields are set; the folders are untouched
 */
void email_imap_roles_capability(const char *response, email_imap_roles_t *out);

/**
 * @brief Read a NAMESPACE response ("* NAMESPACE <personal> <other> <shared>")
 * @param out Zeroed, then filled: the first personal namespace, and the
 *            prefixes of the other-users and shared ones.  No NAMESPACE line
 *            leaves it zeroed (personal prefix "").
 */
void email_imap_namespace_parse(const char *response, email_imap_ns_t *out);

/**
 * @brief Find the Trash and Archive folders in LIST responses
 *
 * A folder marked \Trash (RFC 6154) is the trash; \Archive is the archive,
 * else on Gmail (@p out's gmail set by email_imap_roles_capability) the
 * folder marked \All.  A server that marks neither is matched by name,
 * case-insensitively (Trash, Deleted Items, Deleted Messages, ...; Archive,
 * Archives, ...): the whole name, or the name right under the personal
 * namespace prefix or under INBOX.  A deeper folder of that name is the
 * user's own folder, not the account's.  \Noselect and \NonExistent
 * folders, and anything under another user's or a shared namespace, never
 * count.
 *
 * @param response One or more replies' "* LIST (...) "<delim>" <name>" lines
 * @param ns       The account's namespaces, or NULL when unknown
 * @param out      Its trash and archive fields are set ("" when not found)
 */
void email_imap_roles_from_list(const char *response,
                                const email_imap_ns_t *ns,
                                email_imap_roles_t *out);

/**
 * @brief The delimiter to list INBOX's children with, from a LIST reply
 *
 * A server whose personal namespace is "" can still keep a Trash or Archive
 * under INBOX ("INBOX.Trash"), which a LIST of the top level doesn't show.
 * @return INBOX's delimiter when the reply lists INBOX and doesn't mark it
 *         \HasNoChildren, else '\0'
 */
char email_imap_inbox_child_delim(const char *response);

/**
 * @brief Whether a UID FETCH reply holds message @p uid ("* n FETCH (... UID <uid> ...)")
 */
bool email_imap_fetch_has_uid(const char *response, uint32_t uid);

/**
 * @brief @p folder as an IMAP quoted string ("..." with " and \ escaped)
 * @return true when it fits in @p out_size and holds no line break or NUL
 */
bool email_imap_quote_folder(const char *folder, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_IMAP_ROLES_H */
