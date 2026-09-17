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
       .description =
           "'quote' (live prices — needs symbols), 'portfolio' (holdings + balances across "
           "all the user's Schwab accounts), 'accounts' (balances only), 'history' (price "
           "history + analytics for ONE symbol over a range), or 'fundamentals' (valuation: "
           "P/E, EPS, market cap, dividend yield, 52-week range, beta — one or more symbols).",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "quote", "portfolio", "accounts", "history", "fundamentals" },
       .enum_count = 5,
   },
   {
       .name = "symbols",
       .description = "Ticker symbol(s). For 'quote'/'fundamentals': one or more, "
                      "comma-separated (e.g. 'NVDA' or 'NVDA, AMD, AAPL'). For 'history': a "
                      "single symbol. Ignored for portfolio/accounts.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
   {
       .name = "range",
       .description = "history only: how far back — '1mo','3mo','6mo','1y','5y','ytd' "
                      "(default '1y').",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "range",
       .enum_values = { "1mo", "3mo", "6mo", "1y", "5y", "ytd" },
       .enum_count = 6,
   },
   {
       .name = "interval",
       .description = "history only: candle interval — 'daily','weekly','monthly' (default "
                      "depends on range; an illegal range+interval is auto-corrected).",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "interval",
       .enum_values = { "daily", "weekly", "monthly" },
       .enum_count = 3,
   },
   {
       .name = "data",
       .description = "history only: what to return — 'summary' (computed metrics: % change, "
                      "high/low, volatility, max drawdown, moving averages; default), 'series' "
                      "(compact date+price arrays to chart with render_visual), or 'raw' "
                      "(recent OHLCV candles).",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "data",
       .enum_values = { "summary", "series", "raw" },
       .enum_count = 3,
   },
};

/* ========== Metadata ========== */

static const tool_metadata_t schwab_metadata = {
   .name = "stocks",
   .device_string = "stocks",
   .topic = "dawn",
   .aliases = { "stock" },
   .alias_count = 1,

   .description = "Live stock quotes, the user's Charles Schwab portfolio, price history with "
                  "analytics, and company fundamentals. Actions: 'quote' (prices), 'portfolio' "
                  "/ 'accounts' (holdings + balances), 'history' (price trend + return/"
                  "volatility/drawdown/moving-averages for one symbol; use data='series' then "
                  "render_visual to chart it), 'fundamentals' (P/E, market cap, dividend yield, "
                  "52-week range, beta). Read-only.",
   .params = schwab_params,
   .param_count = 5,

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

   /* The framework appends declared CUSTOM params to the VALUE string as
    * "::field::value"; strip that back to the base symbol string so a `quote`
    * that also carries a stray range= doesn't reach clean_symbols as
    * "NVDA::range::1y". Applies to every symbol-taking action. */
   char syms[256] = "";
   if (value) {
      tool_param_extract_base(value, syms, sizeof(syms));
   }

   if (strcmp(action, "quote") == 0 || strcmp(action, "get") == 0) {
      char *r = schwab_service_quote(user_id, syms);
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
   if (strcmp(action, "history") == 0) {
      char range[16] = "", interval[16] = "", data[16] = "";
      if (value) {
         tool_param_extract_custom(value, "range", range, sizeof(range));
         tool_param_extract_custom(value, "interval", interval, sizeof(interval));
         tool_param_extract_custom(value, "data", data, sizeof(data));
      }
      char *r = schwab_service_history(user_id, syms, range, interval, data);
      return r ? r : oom();
   }
   if (strcmp(action, "fundamentals") == 0) {
      char *r = schwab_service_fundamentals(user_id, syms);
      return r ? r : oom();
   }

   return strdup("Unknown stocks action. Use 'quote', 'portfolio', 'accounts', 'history', or "
                 "'fundamentals'.");
}

/* ========== Public API ========== */

int schwab_tool_register(void) {
   return tool_registry_register(&schwab_metadata);
}
