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
 * An IMAP account's folder roles, probed on the wire and cached: which folder
 * trash and archive move to (email_imap_roles.h parses the replies) and whether
 * the server has MOVE and UIDPLUS.  The destination is the user's own folder
 * for the role: the one the server marks, else a known name, never a guess and
 * never another user's or a shared folder.  The moves themselves are in
 * email_imap_batch.c.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "logging.h"
#include "tools/email_client.h"
#include "tools/email_client_internal.h"
#include "tools/email_imap_roles.h"
#include "tools/email_transfer.h"

/* Roles are probed once per account and reused for an hour; an account found
 * without the role's folder drops them, so the next move looks again (the user
 * may create one).  A move that merely failed keeps them. */
#define ROLES_CACHE_SLOTS 16
#define ROLES_TTL_SEC 3600

typedef struct {
   char key[sizeof(((email_conn_t *)0)->imap_url) + sizeof(((email_conn_t *)0)->username) + 1];
   time_t at;
   email_imap_roles_t roles;
} roles_slot_t;

static roles_slot_t s_roles[ROLES_CACHE_SLOTS];
static pthread_mutex_t s_roles_mutex = PTHREAD_MUTEX_INITIALIZER;

/* An account whose roles probe failed for "all" isn't probed again for a while
 * (one extra failing login per request otherwise); it lists INBOX meanwhile. */
#define ALL_PROBE_RETRY_SEC 300

typedef struct {
   char key[sizeof(((roles_slot_t *)0)->key)];
   time_t at;
} probe_fail_t;

static probe_fail_t s_probe_failed[ROLES_CACHE_SLOTS];

/* The cache key of @p conn's account: its server and login. */
static void roles_key(const email_conn_t *conn, char *key, size_t size) {
   snprintf(key, size, "%s\n%s", conn->imap_url, conn->username);
}

static bool roles_cached(const char *key, email_imap_roles_t *out) {
   const time_t now = time(NULL);
   bool hit = false;
   pthread_mutex_lock(&s_roles_mutex);
   for (int i = 0; i < ROLES_CACHE_SLOTS; i++) {
      if (s_roles[i].key[0] && strcmp(s_roles[i].key, key) == 0 &&
          now - s_roles[i].at < ROLES_TTL_SEC) {
         *out = s_roles[i].roles;
         hit = true;
         break;
      }
   }
   pthread_mutex_unlock(&s_roles_mutex);
   return hit;
}

/* Stores @p roles for @p key (its own slot, else an empty one, else the oldest);
 * NULL @p roles drops the key. */
static void roles_store(const char *key, const email_imap_roles_t *roles) {
   pthread_mutex_lock(&s_roles_mutex);
   int slot = -1;
   for (int i = 0; i < ROLES_CACHE_SLOTS; i++) {
      if (strcmp(s_roles[i].key, key) == 0) {
         slot = i;
         break;
      }
   }
   if (!roles) {
      if (slot >= 0) {
         memset(&s_roles[slot], 0, sizeof(s_roles[slot]));
      }
      pthread_mutex_unlock(&s_roles_mutex);
      return;
   }
   for (int i = 0; slot < 0 && i < ROLES_CACHE_SLOTS; i++) {
      if (!s_roles[i].key[0]) {
         slot = i;
      }
   }
   if (slot < 0) {
      slot = 0;
      for (int i = 1; i < ROLES_CACHE_SLOTS; i++) {
         if (s_roles[i].at < s_roles[slot].at) {
            slot = i;
         }
      }
   }
   snprintf(s_roles[slot].key, sizeof(s_roles[slot].key), "%s", key);
   s_roles[slot].at = time(NULL);
   s_roles[slot].roles = *roles;
   pthread_mutex_unlock(&s_roles_mutex);
}

/* One command on @p curl's URL, its reply in @p buf (the caller frees it);
 * false when the server refused it or it failed.  *first is true while the
 * next command will log in (none has yet, or the last one failed): that one
 * goes through the instrumented perform. */
static bool run(CURL *curl,
                email_instrument_ctx_t *dctx,
                const email_conn_t *conn,
                const char *cmd,
                bool *first,
                curl_buffer_t *buf) {
   const CURLcode res = email_imap_run_command(curl, dctx, conn, "move", cmd, *first, buf);
   if (res != CURLE_OK) {
      OLOG_ERROR("email_imap: %s failed: %s", cmd, curl_easy_strerror(res));
      /* A refused command closes libcurl's connection, so whatever runs next
       * may log in again: send it through the instrumented perform. */
      *first = true;
      return false;
   }
   *first = false;
   return true;
}

/* LIST "" <pattern>, the pattern quoted (a namespace prefix comes from the
 * server). */
static bool list_pattern(CURL *curl,
                         email_instrument_ctx_t *dctx,
                         const email_conn_t *conn,
                         const char *pattern,
                         bool *first,
                         curl_buffer_t *buf) {
   char quoted[2 * (EMAIL_IMAP_NS_PREFIX_MAX + 4) + 3];
   if (!email_imap_quote_folder(pattern, quoted, sizeof(quoted))) {
      memset(buf, 0, sizeof(*buf));
      return false;
   }
   char cmd[sizeof(quoted) + 16];
   snprintf(cmd, sizeof(cmd), "LIST \"\" %s", quoted);
   return run(curl, dctx, conn, cmd, first, buf);
}

/* The @p n replies joined, one line break between; NULL when out of memory. */
static char *join_replies(const curl_buffer_t *bufs, int n) {
   size_t total = 1;
   for (int i = 0; i < n; i++) {
      total += (bufs[i].data ? bufs[i].size : 0) + 1;
   }
   char *out = malloc(total);
   if (!out) {
      return NULL;
   }
   size_t o = 0;
   for (int i = 0; i < n; i++) {
      if (bufs[i].data) {
         memcpy(out + o, bufs[i].data, bufs[i].size);
         o += bufs[i].size;
      }
      out[o++] = '\n';
   }
   out[o] = '\0';
   return out;
}

/* The account's roles, probed on @p curl when not cached: CAPABILITY, then
 * NAMESPACE, then the marked folders (LIST (SPECIAL-USE)) and the folders one
 * level under the personal namespace (where a Trash or Archive of a server
 * that marks nothing lives).  Never a LIST of the whole tree: a big shared
 * tree would outgrow the reply cap, and nothing deep in it can be a target. */
static int roles_for(CURL *curl,
                     email_instrument_ctx_t *dctx,
                     const email_conn_t *conn,
                     const char *key,
                     bool *first,
                     email_imap_roles_t *roles) {
   if (roles_cached(key, roles)) {
      return 0;
   }
   memset(roles, 0, sizeof(*roles));
   curl_easy_setopt(curl, CURLOPT_URL, conn->imap_url);
   curl_buffer_t cap;
   if (!run(curl, dctx, conn, "CAPABILITY", first, &cap) || !cap.data) {
      curl_buffer_free(&cap);
      return 1;
   }
   email_imap_roles_capability(cap.data, roles);
   curl_buffer_free(&cap);

   email_imap_ns_t ns;
   memset(&ns, 0, sizeof(ns));
   if (roles->namespace_) {
      curl_buffer_t nsb;
      if (run(curl, dctx, conn, "NAMESPACE", first, &nsb) && nsb.data) {
         email_imap_namespace_parse(nsb.data, &ns);
      }
      curl_buffer_free(&nsb);
   }

   /* The marked folders, then the level under the personal prefix, then (when
    * that prefix is "") INBOX's children. */
   curl_buffer_t lists[3];
   memset(lists, 0, sizeof(lists));
   int rc = 1;
   if (roles->special_use &&
       !run(curl, dctx, conn, "LIST (SPECIAL-USE) \"\" \"*\"", first, &lists[0])) {
      curl_buffer_free(&lists[0]); /* the level list below still finds marked folders there */
   }
   char pattern[EMAIL_IMAP_NS_PREFIX_MAX + 4];
   snprintf(pattern, sizeof(pattern), "%s%%", ns.personal);
   if (!list_pattern(curl, dctx, conn, pattern, first, &lists[1])) {
      goto out;
   }
   const char inbox_delim = ns.personal[0] ? '\0' : email_imap_inbox_child_delim(lists[1].data);
   if (inbox_delim) {
      snprintf(pattern, sizeof(pattern), "INBOX%c%%", inbox_delim);
      if (!list_pattern(curl, dctx, conn, pattern, first, &lists[2])) {
         curl_buffer_free(&lists[2]); /* a top-level Trash or Archive still counts */
      }
   }
   char *joined = join_replies(lists, 3);
   if (!joined) {
      goto out;
   }
   email_imap_roles_from_list(joined, &ns, roles);
   free(joined);
   rc = 0;

out:
   for (int i = 0; i < 3; i++) {
      curl_buffer_free(&lists[i]);
   }
   if (rc != 0) {
      return rc;
   }
   OLOG_INFO("email_imap: folder roles for this account: trash=\"%s\" archive=\"%s\" move=%d "
             "uidplus=%d",
             roles->trash, roles->archive, roles->move, roles->uidplus);
   roles_store(key, roles);
   return 0;
}

/* Logs in on @p curl's own (no mailbox selected), so a refused login is told
 * apart from a refused SELECT, then the account's roles. */
email_err_t email_imap_open_roles(CURL *curl,
                                  email_instrument_ctx_t *dctx,
                                  const email_conn_t *conn,
                                  email_imap_roles_t *roles) {
   curl_easy_setopt(curl, CURLOPT_URL, conn->imap_url);
   curl_buffer_t buf;
   const CURLcode res = email_imap_run_command(curl, dctx, conn, "move", "NOOP", true, &buf);
   curl_buffer_free(&buf);
   if (res != CURLE_OK)
      return email_err_from_curl(res);
   char key[sizeof(((roles_slot_t *)0)->key)];
   roles_key(conn, key, sizeof(key));
   bool first = false;
   return roles_for(curl, dctx, conn, key, &first, roles) == 0 ? EMAIL_ERR_NONE : EMAIL_ERR_FAILED;
}

void email_imap_roles_forget(const email_conn_t *conn) {
   char key[sizeof(((roles_slot_t *)0)->key)];
   roles_key(conn, key, sizeof(key));
   roles_store(key, NULL);
}

/* With s_roles_mutex held: @p key's probe failed within ALL_PROBE_RETRY_SEC. */
static bool probe_failed_recently_locked(const char *key, time_t now) {
   for (int i = 0; i < ROLES_CACHE_SLOTS; i++) {
      if (s_probe_failed[i].key[0] && strcmp(s_probe_failed[i].key, key) == 0)
         return now - s_probe_failed[i].at < ALL_PROBE_RETRY_SEC;
   }
   return false;
}

/* Records (@p failed) or clears @p key's failed probe. */
static void probe_note(const char *key, bool failed) {
   const time_t now = time(NULL);
   pthread_mutex_lock(&s_roles_mutex);
   int slot = -1, oldest = 0;
   for (int i = 0; i < ROLES_CACHE_SLOTS; i++) {
      if (s_probe_failed[i].key[0] && strcmp(s_probe_failed[i].key, key) == 0)
         slot = i;
      if (s_probe_failed[i].at < s_probe_failed[oldest].at)
         oldest = i;
   }
   if (!failed) {
      if (slot >= 0)
         memset(&s_probe_failed[slot], 0, sizeof(s_probe_failed[slot]));
   } else {
      if (slot < 0)
         slot = oldest; /* an empty slot has at == 0, so it's the oldest */
      snprintf(s_probe_failed[slot].key, sizeof(s_probe_failed[slot].key), "%s", key);
      s_probe_failed[slot].at = now;
   }
   pthread_mutex_unlock(&s_roles_mutex);
}

bool email_imap_all_mail_folder(const email_conn_t *conn, char *out, size_t size) {
   if (!conn || !out || size == 0)
      return false;
   out[0] = '\0';
   char key[sizeof(((roles_slot_t *)0)->key)];
   roles_key(conn, key, sizeof(key));
   email_imap_roles_t roles;
   if (!roles_cached(key, &roles)) {
      pthread_mutex_lock(&s_roles_mutex);
      const bool skip = probe_failed_recently_locked(key, time(NULL));
      pthread_mutex_unlock(&s_roles_mutex);
      if (skip)
         return false;
      CURL *curl = email_imap_handle_create(conn);
      if (!curl)
         return false;
      email_instrument_ctx_t dctx;
      email_instrument_attach(curl, &dctx);
      bool first = true;
      const int rc = roles_for(curl, &dctx, conn, key, &first, &roles);
      email_instrument_op_done(conn->username, "roles", curl, &dctx);
      curl_easy_cleanup(curl);
      probe_note(key, rc != 0);
      if (rc != 0)
         return false;
   }
   if (!roles.all[0] || strlen(roles.all) >= size)
      return false;
   snprintf(out, size, "%s", roles.all);
   return true;
}
