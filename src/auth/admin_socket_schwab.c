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
 * Charles Schwab OAuth enrollment operator opcode handlers (dawn-admin schwab
 * auth-url / auth-complete / status) — the CLI manual-paste bootstrap
 * (docs/SCHWAB_SETUP.md).  Sibling of admin_socket_research.c.
 *
 * Schwab pins its redirect URI to the developer-app registration and cannot use
 * DAWN's WebUI callback without HTTPS + re-registration, so round one enrolls
 * over this SO_PEERCRED-gated admin socket: `auth-url` mints an authorize URL,
 * the operator approves in a browser and pastes the resulting redirect URL to
 * `auth-complete`, which validates + exchanges + stores the tokens.  The daemon
 * then auto-refreshes until Schwab's fixed 7-day refresh token lapses.  --user
 * is REQUIRED and validated against a real account so a link never lands under a
 * bogus uid.  Neither the URL nor the code is ever logged.
 */

#define ADMIN_SOCKET_INTERNAL_ALLOWED

#ifdef DAWN_ENABLE_SCHWAB_TOOL

#include <sodium.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "auth/admin_socket_internal.h"
#include "auth/auth_db.h"
#include "logging.h"
#include "tools/oauth_client.h"
#include "tools/schwab_client.h" /* SCHWAB_ACCOUNT_KEY (one canonical definition) */

/* Schwab's refresh token has a fixed 7-day lifetime from the original auth. */
#define SCHWAB_REFRESH_LIFETIME_DAYS 7

/* Little-endian reader — the wire is fixed LE regardless of host byte order. */
static int32_t rd_i32le(const unsigned char *p) {
   return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                    ((uint32_t)p[3] << 24));
}

/* Validate the required --user against a real account; returns 0 on success and
 * fills ident, else sends the failure response and returns non-zero. */
static int require_valid_user(int client_fd,
                              const unsigned char *p,
                              uint16_t payload_len,
                              int *user_id_out,
                              auth_user_identity_t *ident) {
   if (payload_len < 4) {
      send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid payload (need user_id).");
      return 1;
   }
   int user_id = (int)rd_i32le(p);
   if (user_id <= 0) {
      send_text_response(client_fd, ADMIN_RESP_FAILURE, "A valid --user is required.");
      return 1;
   }
   if (auth_db_get_user_identity(user_id, ident) != AUTH_DB_SUCCESS) {
      send_text_response(client_fd, ADMIN_RESP_NOT_FOUND, "No such user.");
      return 1;
   }
   *user_id_out = user_id;
   return 0;
}

/* =============================================================================
 * schwab auth-url — mint an authorize URL.  Payload: [user_id i32].
 * ============================================================================= */

int handle_schwab_auth_url_cmd(int client_fd, const char *payload, uint16_t payload_len) {
   int user_id = 0;
   auth_user_identity_t ident;
   if (require_valid_user(client_fd, (const unsigned char *)payload, payload_len, &user_id,
                          &ident) != 0) {
      return 0;
   }

   oauth_provider_config_t provider;
   if (oauth_build_schwab_provider(&provider) != 0) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "Schwab is not configured — set client_id/client_secret in "
                                "secrets.toml [secrets.schwab].");
   }

   char url[2048];
   char state[128];
   if (oauth_get_auth_url(&provider, user_id, url, sizeof(url), state, sizeof(state)) != 0) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR,
                                "Could not generate the authorization URL (too many pending "
                                "flows? try again in a few minutes).");
   }

   OLOG_INFO("schwab: admin minted an authorize URL for user %d", user_id);
   /* Return only the URL; dawn-admin prints the human instructions + deadline. */
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, url);
}

/* =============================================================================
 * schwab auth-complete — finish enrollment from the pasted redirect URL.
 * Payload: [user_id i32][redirect-URL bytes].
 * ============================================================================= */

int handle_schwab_auth_complete_cmd(int client_fd, const char *payload, uint16_t payload_len) {
   int user_id = 0;
   auth_user_identity_t ident;
   if (require_valid_user(client_fd, (const unsigned char *)payload, payload_len, &user_id,
                          &ident) != 0) {
      return 0;
   }
   if (payload_len < 5) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Missing the pasted redirect URL.");
   }

   /* Copy the URL out into a NUL-terminated buffer. */
   uint16_t url_len = (uint16_t)(payload_len - 4);
   char url[2048];
   if (url_len >= sizeof(url)) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Redirect URL too long.");
   }
   memcpy(url, payload + 4, url_len);
   url[url_len] = '\0';

   oauth_provider_config_t provider;
   if (oauth_build_schwab_provider(&provider) != 0) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "Schwab is not configured — set client_id/client_secret in "
                                "secrets.toml [secrets.schwab].");
   }

   char err[256];
   int rc = oauth_complete_from_redirect(&provider, user_id, url, SCHWAB_ACCOUNT_KEY, err,
                                         sizeof(err));
   sodium_memzero(url, sizeof(url));
   if (rc != 0) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, err[0] ? err : "Enrollment failed.");
   }

   OLOG_INFO("schwab: linked account for user %d", user_id);
   char msg[256];
   snprintf(msg, sizeof(msg),
            "Schwab linked for user %d%s%s. Access auto-refreshes; re-link within %d days.",
            user_id, ident.real_name[0] ? " " : "", ident.real_name, SCHWAB_REFRESH_LIFETIME_DAYS);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}

/* =============================================================================
 * schwab status — link state + refresh-expiry countdown.  Payload: [user_id i32].
 * ============================================================================= */

int handle_schwab_status_cmd(int client_fd, const char *payload, uint16_t payload_len) {
   int user_id = 0;
   auth_user_identity_t ident;
   if (require_valid_user(client_fd, (const unsigned char *)payload, payload_len, &user_id,
                          &ident) != 0) {
      return 0;
   }

   (void)ident; /* validated for existence; reported by id */
   oauth_token_set_t tokens;
   char msg[256];
   if (oauth_load_tokens(user_id, "schwab", SCHWAB_ACCOUNT_KEY, &tokens) != 0) {
      snprintf(msg, sizeof(msg),
               "Schwab is not linked for user %d. Run: dawn-admin schwab auth --user %d", user_id,
               user_id);
      return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
   }

   if (tokens.linked_at > 0) {
      int64_t age_days = (time(NULL) - tokens.linked_at) / 86400;
      int64_t days_left = SCHWAB_REFRESH_LIFETIME_DAYS - age_days;
      if (days_left < 0) {
         days_left = 0;
      }
      snprintf(msg, sizeof(msg),
               "Schwab linked for user %d. Refresh token expires in ~%lld day(s)%s.", user_id,
               (long long)days_left,
               days_left == 0 ? " — re-link now (dawn-admin schwab auth)" : "");
   } else {
      snprintf(msg, sizeof(msg),
               "Schwab linked for user %d. Refresh token expires within %d days of linking.",
               user_id, SCHWAB_REFRESH_LIFETIME_DAYS);
   }
   sodium_memzero(&tokens, sizeof(tokens));
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}

#endif /* DAWN_ENABLE_SCHWAB_TOOL */
