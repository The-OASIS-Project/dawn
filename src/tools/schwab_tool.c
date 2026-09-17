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
 * Charles Schwab stocks tool — the LLM surface for quotes + portfolio (read-only).
 * Metadata + dispatch only; all API/formatting lives in schwab_service.c. A later
 * round adds trading as a SEPARATE, DANGEROUS-gated tool (docs/SCHWAB_SETUP.md).
 */

#include "tools/schwab_tool.h"

#include <stdlib.h>
#include <string.h>

#include "logging.h"
#include "tools/schwab_service.h"
#include "tools/tool_registry.h"

static char *schwab_tool_callback(const char *action, char *value, int *should_respond);

/* ========== Parameters ========== */

static const treg_param_t schwab_params[] = {
   {
       .name = "action",
       .description = "'quote' (live prices — requires symbols), 'portfolio' (the user's "
                      "holdings and balances across all their Schwab accounts), or "
                      "'accounts' (balances only, no positions).",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "quote", "portfolio", "accounts" },
       .enum_count = 3,
   },
   {
       .name = "symbols",
       .description = "For 'quote' only: one or more ticker symbols, comma-separated "
                      "(e.g. 'NVDA' or 'NVDA, AMD, AAPL'). Ignored for portfolio/accounts.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

/* ========== Metadata ========== */

static const tool_metadata_t schwab_metadata = {
   .name = "stocks",
   .device_string = "stocks",
   .topic = "dawn",
   .aliases = { "stock" },
   .alias_count = 1,

   .description = "Look up live stock quotes and the user's Charles Schwab portfolio. Use "
                  "'quote' with one or more ticker symbols for prices; 'portfolio' for the "
                  "user's holdings and balances across all their Schwab accounts; 'accounts' "
                  "for balances only. Read-only.",
   .params = schwab_params,
   .param_count = 2,

   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_NETWORK | TOOL_CAP_SECRETS | TOOL_CAP_INFORMATIONAL |
                   TOOL_CAP_SCHEDULABLE,
   .default_local = true,
   .default_remote = true,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,
   .secret_requirements = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = schwab_tool_callback,
};

/* ========== Callback ========== */

static char *oom(void) {
   return strdup(TOOL_RESULT_ERROR_MARK "Stocks: out of memory.");
}

static char *schwab_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1;
   int user_id = tool_get_current_user_id();

   if (!action) {
      action = "quote";
   }

   if (strcmp(action, "quote") == 0 || strcmp(action, "get") == 0) {
      /* `symbols` maps to VALUE, so `value` is the raw symbols string. */
      char *r = schwab_service_quote(user_id, value);
      return r ? r : oom();
   }
   if (strcmp(action, "portfolio") == 0 || strcmp(action, "positions") == 0) {
      char *r = schwab_service_portfolio(user_id, false /* full detail */);
      return r ? r : oom();
   }
   if (strcmp(action, "accounts") == 0) {
      char *r = schwab_service_portfolio(user_id, true /* balances only */);
      return r ? r : oom();
   }

   return strdup("Unknown stocks action. Use 'quote' (with symbols), 'portfolio', or 'accounts'.");
}

/* ========== Public API ========== */

int schwab_tool_register(void) {
   return tool_registry_register(&schwab_metadata);
}
