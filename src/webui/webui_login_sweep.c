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
 * Ending logins in the WebUI.  However a login ends (logout, a login over
 * it, a revoke from the WebUI or dawn-admin, a password change, a deleted
 * user, expiry), the database is changed first and then one sweep runs on
 * the service thread: every browser connection whose login no longer
 * resolves is told (force_logout), stripped of its identity and closed with
 * WEBUI_CLOSE_LOGGED_OUT, and every session owned by a login that no longer
 * exists is destroyed, connected or not.  The database is the one authority,
 * so no caller has to say which connections or sessions it meant.
 */

#include <stdatomic.h>
#include <string.h>

#include "auth/auth_db.h"
#include "core/session_manager.h"
#include "logging.h"
#include "webui/webui_internal.h"
#include "webui/webui_send.h"

_Static_assert(SESSION_OWNER_KEY_LEN == AUTH_TOKEN_PREFIX_LEN,
               "a session's owner key is its login's public token prefix");

static atomic_bool s_sweep_pending;

void webui_conn_owner_key(const ws_connection_t *conn, char out[SESSION_OWNER_KEY_LEN + 1]) {
   out[0] = '\0';
   if (conn->authenticated && strlen(conn->auth_session_token) >= SESSION_OWNER_KEY_LEN) {
      memcpy(out, conn->auth_session_token, SESSION_OWNER_KEY_LEN);
      out[SESSION_OWNER_KEY_LEN] = '\0';
   }
}

void webui_conn_own_session(ws_connection_t *conn, session_t *session) {
   char key[SESSION_OWNER_KEY_LEN + 1];
   webui_conn_owner_key(conn, key);
   if (key[0]) {
      (void)session_set_owner(session, key);
   }
}

bool webui_conn_may_resume(ws_connection_t *conn, session_t *session) {
   if (session->type != SESSION_TYPE_WEBUI) {
      return false; /* a satellite's session is resumed by its registration */
   }
   char key[SESSION_OWNER_KEY_LEN + 1];
   webui_conn_owner_key(conn, key);
   return session_owner_matches(session, key);
}

bool webui_conn_attach_session(ws_connection_t *conn, session_t *session) {
   /* Under the lock webui_detach_session() scans with, after the flag its
    * destroy sets first: a destroy either finds this connection attached (and
    * detaches it) or is seen here. */
   pthread_mutex_lock(&s_conn_registry_mutex);
   const bool live = !atomic_load(&session->being_destroyed);
   if (live) {
      conn_set_session(conn, session);
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);
   return live;
}

bool webui_conn_end_login(ws_connection_t *conn, const char *reason) {
   if (!conn || conn->logged_out) {
      return false;
   }
   conn->logged_out = true;

   /* Its session, if this login owns it, ends with the login. */
   char key[SESSION_OWNER_KEY_LEN + 1];
   webui_conn_owner_key(conn, key);
   session_t *session = conn_get_session(conn);
   const uint32_t owned_id = session && key[0] && session_owner_is(session, key)
                                 ? session->session_id
                                 : 0;

   /* The frame first (a proxy strips the close code; the frame gets through),
    * unless a large frame is still going out, which it would interleave with:
    * then the close code alone says it. */
   if (conn->wsi && !lws_partial_buffered(conn->wsi)) {
      send_force_logout_impl(conn->wsi, reason);
   }

   conn->authenticated = false;
   conn->auth_user_id = 0;
   explicit_bzero(conn->auth_session_token, sizeof(conn->auth_session_token));
   memset(conn->username, 0, sizeof(conn->username));
   atomic_store(&conn->active_conversation_id, 0);
   conn->active_conversation_private = false;

   if (conn->wsi) {
      /* Closed from its next writeable callback, once the frame is out
       * (callback_websocket); the timeout closes it if that never comes. */
      /* An application close code (4000-4999), which lws passes through */
      // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
      lws_close_reason(conn->wsi, (enum lws_close_status)WEBUI_CLOSE_LOGGED_OUT,
                       (unsigned char *)"logged out", 10);
      lws_callback_on_writable(conn->wsi);
      lws_set_timeout(conn->wsi, PENDING_TIMEOUT_CLOSE_SEND, 3);
   }
   if (owned_id > 0) {
      session_destroy(owned_id);
   }
   return owned_id > 0;
}

bool webui_conn_login_valid(ws_connection_t *conn) {
   if (!conn->authenticated) {
      return false;
   }
   auth_session_t login;
   const int rc = auth_db_get_session(conn->auth_session_token, &login);
   if (rc == AUTH_DB_SUCCESS) {
      return true;
   }
   if (rc == AUTH_DB_NOT_FOUND) {
      OLOG_INFO("WebUI: connection's login has ended; closing it");
      webui_conn_end_login(conn, "Signed out");
   } else {
      send_error_impl(conn->wsi, "SERVICE_UNAVAILABLE", "Couldn't check the login; try again");
   }
   return false;
}

/* End what belongs to logins the database no longer has: close their browser
 * connections and destroy their sessions, connected or not.  Service thread
 * (webui_login_sweep_run_pending). */
static void login_sweep(void) {
   /* Browser connections whose login no longer resolves.  Collected under the
    * registry lock; used after it on this thread, where connections are
    * freed (their close runs here too). */
   ws_connection_t *conns[MAX_ACTIVE_CONNECTIONS];
   int n = 0;
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *c = s_active_connections[i];
      if (c && c->wsi && c->authenticated && !c->is_satellite && !c->logged_out) {
         conns[n++] = c;
      }
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);

   /* One lookup per login (several tabs share one); only a login the database
    * says is gone ends: a failed lookup ends nothing. */
   bool gone[MAX_ACTIVE_CONNECTIONS] = { false };
   int closed = 0;
   int destroyed = 0; /* sessions: the closed connections' own, then the rest */
   for (int i = 0; i < n; i++) {
      int same = -1;
      for (int j = 0; j < i && same < 0; j++) {
         if (strcmp(conns[j]->auth_session_token, conns[i]->auth_session_token) == 0) {
            same = j;
         }
      }
      if (same >= 0) {
         gone[i] = gone[same];
      } else {
         auth_session_t login;
         gone[i] = auth_db_get_session(conns[i]->auth_session_token, &login) == AUTH_DB_NOT_FOUND;
      }
   }
   for (int i = 0; i < n; i++) {
      if (gone[i]) {
         if (webui_conn_end_login(conns[i], "Signed out")) {
            destroyed++;
         }
         closed++;
      }
   }

   /* Sessions owned by a login that no longer exists, connected or not. */
   session_owned_t owned[MAX_SESSIONS];
   const int m = session_manager_list_owned(owned, MAX_SESSIONS);
   for (int i = 0; i < m; i++) {
      bool exists = true;
      if (auth_db_session_prefix_exists(owned[i].owner_key, &exists) == AUTH_DB_SUCCESS &&
          !exists) {
         session_destroy(owned[i].session_id);
         destroyed++;
      }
   }
   if (closed > 0 || destroyed > 0) {
      OLOG_INFO("WebUI: ended logins: %d connection(s) closed, %d session(s) destroyed", closed,
                destroyed);
   }
}

void webui_login_sweep_request(void) {
   atomic_store(&s_sweep_pending, true);
   if (s_lws_context) {
      lws_cancel_service(s_lws_context);
   }
}

void webui_login_sweep_run_pending(void) {
   if (atomic_exchange(&s_sweep_pending, false)) {
      login_sweep();
   }
}

/* auth_db's hook (a weak no-op there): logins changed in the database, on
 * whatever thread changed them; the sweep runs on the service thread. */
void auth_sessions_changed(void) {
   webui_login_sweep_request();
}
