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
 * Charles Schwab REST client — thin authenticated JSON GET over libcurl.
 */

#ifndef SCHWAB_CLIENT_H
#define SCHWAB_CLIENT_H

#include <json-c/json.h>

/* OAuth storage key for the single per-user Schwab link (the multiple brokerage
 * accounts are resolved from the API, not from multiple OAuth links). */
#define SCHWAB_ACCOUNT_KEY "default"

/* Schwab splits market-data and trader/accounts under one host. */
#define SCHWAB_MARKETDATA_BASE "https://api.schwabapi.com/marketdata/v1"
#define SCHWAB_TRADER_BASE "https://api.schwabapi.com/trader/v1"

typedef enum {
   SCHWAB_RC_OK = 0,
   SCHWAB_RC_ERROR = 1,        /**< generic (config/parse/internal) */
   SCHWAB_RC_NOT_LINKED = 2,   /**< no stored token for this user */
   SCHWAB_RC_AUTH = 3,         /**< token refresh failed / HTTP 401 */
   SCHWAB_RC_HTTP = 4,         /**< network/transport or non-200 status */
   SCHWAB_RC_RATE_LIMITED = 5, /**< HTTP 429 */
   SCHWAB_RC_TOO_LARGE = 6,    /**< HTTP 200 but the body exceeded SCHWAB_MAX_RESPONSE */
} schwab_rc_t;

/**
 * Authenticated GET returning parsed JSON.
 * On HTTP 200 with valid JSON, *out_root receives an owned json_object the caller
 * must json_object_put(); otherwise *out_root is NULL. *http_code_out (optional)
 * receives the HTTP status. Never logs the bearer token or response body.
 */
schwab_rc_t schwab_client_get_json(const char *bearer,
                                   const char *url,
                                   struct json_object **out_root,
                                   long *http_code_out);

#endif /* SCHWAB_CLIENT_H */
