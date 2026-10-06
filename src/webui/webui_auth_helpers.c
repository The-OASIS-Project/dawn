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
 * WebUI authentication helpers: the per-connection auth gates
 * (`conn_require_auth`, `conn_require_admin`) every WebSocket message handler
 * uses.  Split out of webui_server.c so that file can stay under the size
 * limits in CLAUDE.md.  (The prompt builder is core/prompt_builder.c.)
 *
 * Whole-file compilation gated on ENABLE_AUTH.
 */

#include "auth/auth_db.h"
#include "logging.h"
#include "webui/webui_internal.h"
#include "webui/webui_server.h"

/* =============================================================================
 * Authentication Helpers
 * ============================================================================= */

#ifdef ENABLE_AUTH

/* HTTP auth helpers (extract_session_cookie, is_request_authenticated)
 * moved to webui_http.c */

/**
 * @brief Check if WebSocket connection is authenticated
 *
 * CRITICAL: Re-validates session against database to prevent TOCTOU attacks
 * where session may have been revoked (password change, admin action, etc.)
 * but cached conn->authenticated flag remains true.
 *
 * Sends UNAUTHORIZED error if not authenticated or session invalid.
 *
 * @param conn WebSocket connection
 * @return true if authenticated with valid session, false otherwise (error sent)
 */
bool conn_require_auth_ex(ws_connection_t *conn, auth_session_t *session_out) {
   if (!conn->authenticated) {
      send_error_impl(conn->wsi, "UNAUTHORIZED", "Authentication required");
      return false;
   }

   /* Re-validate session from DB (prevents stale session exploitation) */
   auth_session_t session;
   if (auth_db_get_session(conn->auth_session_token, &session) != AUTH_DB_SUCCESS) {
      conn->authenticated = false;
      send_error_impl(conn->wsi, "UNAUTHORIZED", "Session expired or revoked");
      return false;
   }

   /* Hand the freshly-fetched session to the caller so it needn't re-SELECT
    * (e.g. handle_ping renewing keepalive from the same read). */
   if (session_out) {
      *session_out = session;
   }
   return true;
}

bool conn_require_auth(ws_connection_t *conn) {
   return conn_require_auth_ex(conn, NULL);
}

/**
 * @brief Check if WebSocket connection has admin privileges
 *
 * CRITICAL: Re-validates is_admin against database to prevent stale cache
 * exploitation if user is demoted mid-session.
 *
 * Sends UNAUTHORIZED if not authenticated, FORBIDDEN if not admin.
 *
 * @param conn WebSocket connection
 * @return true if admin, false otherwise (error sent)
 */
bool conn_require_admin(ws_connection_t *conn) {
   if (!conn->authenticated) {
      send_error_impl(conn->wsi, "UNAUTHORIZED", "Authentication required");
      return false;
   }

   /* Re-validate session from DB (prevents stale is_admin cache) */
   auth_session_t session;
   if (auth_db_get_session(conn->auth_session_token, &session) != AUTH_DB_SUCCESS) {
      conn->authenticated = false;
      send_error_impl(conn->wsi, "UNAUTHORIZED", "Session expired");
      return false;
   }

   if (!session.is_admin) {
      auth_db_log_event("PERMISSION_DENIED", conn->username, conn->client_ip,
                        "Admin access required");
      send_error_impl(conn->wsi, "FORBIDDEN", "Admin access required");
      return false;
   }

   return true;
}

#endif /* ENABLE_AUTH */
