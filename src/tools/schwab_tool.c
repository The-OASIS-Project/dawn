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

#include "dawn_error.h"
#include "logging.h"
#include "tools/schwab_service.h"
#include "tools/schwab_watchlist.h"
#include "tools/tool_registry.h"

static char *schwab_tool_callback(const char *action, char *value, int *should_respond);

/* ========== Parameters ========== */

static const treg_param_t schwab_params[] = {
   {
       .name = "action",
       .description =
           "'quote' (live prices — needs symbols), 'portfolio' (holdings + balances + "
           "unrealized P/L across all the user's Schwab accounts), 'accounts' (balances only), "
           "'history' (price history + analytics for ONE symbol over a range), 'fundamentals' "
           "(valuation: P/E, EPS, market cap, dividend yield, 52-week range, beta — one or more "
           "symbols), 'transactions' (recent account activity: trades, dividends, deposits/"
           "withdrawals, fees — over a date window), or the watchlist actions 'watch_add' / "
           "'watch_remove' (symbols in the value) / 'watch_list' (show the tracked tickers).",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "quote", "portfolio", "accounts", "history", "fundamentals", "transactions",
                        "watch_add", "watch_remove", "watch_list" },
       .enum_count = 9,
   },
   {
       .name = "symbols",
       .description = "Ticker symbol(s). For 'quote'/'fundamentals': one or more, "
                      "comma-separated (e.g. 'NVDA' or 'NVDA, AMD, AAPL'). For 'history': a "
                      "single symbol. For 'transactions': an optional single-symbol filter "
                      "(note: won't reliably match dividend rows). Ignored for "
                      "portfolio/accounts.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
   {
       .name = "range",
       .description = "history only: how far back — '1mo','3mo','6mo','1y','5y','ytd' "
                      "(default '1y'). Ignored when 'start' is given.",
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
   {
       .name = "start",
       .description = "history/transactions: custom start date YYYY-MM-DD (e.g. '2025-03-01'). "
                      "For history, overrides 'range' with an explicit window. For "
                      "transactions, defaults to the last ~60 days if omitted (Schwab serves "
                      "at most ~1 year back).",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "start",
   },
   {
       .name = "end",
       .description = "history/transactions: custom end date YYYY-MM-DD (default: today). Only "
                      "used together with 'start'.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "end",
   },
   {
       .name = "type",
       .description = "transactions only: filter by category — 'trades', 'dividends' (incl. "
                      "interest), 'deposits', 'withdrawals', 'fees', or 'all' (default).",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "type",
       .enum_values = { "trades", "dividends", "deposits", "withdrawals", "fees", "all" },
       .enum_count = 6,
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
                  "analytics, company fundamentals, and recent account transactions. Actions: "
                  "'quote' (prices), 'portfolio' / 'accounts' (holdings + balances + unrealized "
                  "P/L), 'history' (price trend + return/volatility/drawdown/moving-averages for "
                  "one symbol; use data='series' then render_visual to chart it), 'fundamentals' "
                  "(P/E, market cap, dividend yield, 52-week range, beta), 'transactions' "
                  "(recent trades/dividends/deposits/withdrawals/fees over a date window). "
                  "Read-only.",
   .params = schwab_params,
   .param_count = 8,

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
      char range[16] = "", interval[16] = "", data[16] = "", start[16] = "", end[16] = "";
      if (value) {
         tool_param_extract_custom(value, "range", range, sizeof(range));
         tool_param_extract_custom(value, "interval", interval, sizeof(interval));
         tool_param_extract_custom(value, "data", data, sizeof(data));
         tool_param_extract_custom(value, "start", start, sizeof(start));
         tool_param_extract_custom(value, "end", end, sizeof(end));
      }
      char *r = schwab_service_history(user_id, syms, range, interval, data, start, end);
      return r ? r : oom();
   }
   if (strcmp(action, "fundamentals") == 0) {
      char *r = schwab_service_fundamentals(user_id, syms);
      return r ? r : oom();
   }
   if (strcmp(action, "transactions") == 0) {
      char start[16] = "", end[16] = "", type[16] = "";
      if (value) {
         tool_param_extract_custom(value, "start", start, sizeof(start));
         tool_param_extract_custom(value, "end", end, sizeof(end));
         tool_param_extract_custom(value, "type", type, sizeof(type));
      }
      /* syms is the optional single-symbol filter (base-extracted above). */
      char *r = schwab_service_transactions(user_id, syms, start, end, type);
      return r ? r : oom();
   }
   if (strcmp(action, "watch_remove") == 0) {
      char work[256];
      snprintf(work, sizeof(work), "%s", syms);
      int done = 0;
      char *save = NULL;
      for (char *tok = strtok_r(work, " ,\t", &save); tok; tok = strtok_r(NULL, " ,\t", &save)) {
         if (schwab_watchlist_remove(user_id, tok) == SUCCESS) {
            done++;
         }
      }
      if (done == 0) {
         return strdup("No symbols given for the watchlist.");
      }
      webui_stocks_watchlist_invalidate(user_id);
      char buf[128];
      snprintf(buf, sizeof(buf), "Removed %d symbol%s from the watchlist.", done,
               done == 1 ? "" : "s");
      return strdup(buf);
   }
   if (strcmp(action, "watch_add") == 0) {
      /* Validate-then-add: normalize, check against Schwab's invalidSymbols, store
       * only real tickers. Runs on a worker thread, so a blocking Schwab call is OK. */
      char work[256];
      snprintf(work, sizeof(work), "%s", syms);
      char norm[SCHWAB_WATCHLIST_MAX][SCHWAB_SYMBOL_MAX];
      int nn = 0, badchars = 0;
      char *save = NULL;
      for (char *tok = strtok_r(work, " ,\t", &save); tok; tok = strtok_r(NULL, " ,\t", &save)) {
         char n[SCHWAB_SYMBOL_MAX];
         if (nn < SCHWAB_WATCHLIST_MAX &&
             schwab_watchlist_normalize(tok, n, sizeof(n)) == SUCCESS) {
            snprintf(norm[nn++], SCHWAB_SYMBOL_MAX, "%s", n);
         } else {
            badchars++;
         }
      }
      if (nn == 0 && badchars == 0) {
         return strdup("No symbols given for the watchlist.");
      }
      char invalid[SCHWAB_WATCHLIST_MAX][SCHWAB_SYMBOL_MAX];
      int ninv = 0;
      schwab_rc_t rc = SCHWAB_RC_OK;
      if (nn > 0) {
         const char *ptrs[SCHWAB_WATCHLIST_MAX];
         for (int i = 0; i < nn; i++) {
            ptrs[i] = norm[i];
         }
         rc = schwab_service_validate_symbols(user_id, ptrs, nn, invalid, SCHWAB_WATCHLIST_MAX,
                                              &ninv);
      }
      int added = 0, unknown = 0, full = 0;
      for (int i = 0; i < nn; i++) {
         bool bad = false;
         if (rc == SCHWAB_RC_OK) {
            for (int j = 0; j < ninv; j++) {
               if (strcmp(norm[i], invalid[j]) == 0) {
                  bad = true;
                  break;
               }
            }
         }
         if (bad) {
            unknown++;
            continue;
         }
         schwab_watch_add_t r = schwab_watchlist_add(user_id, norm[i]);
         if (r == SCHWAB_WATCH_ADDED || r == SCHWAB_WATCH_DUPLICATE) {
            added++;
         } else if (r == SCHWAB_WATCH_FULL) {
            full++;
         }
      }
      if (added > 0) {
         webui_stocks_watchlist_invalidate(user_id);
      }
      char buf[256];
      size_t off = (size_t)snprintf(buf, sizeof(buf), "Added %d symbol%s to the watchlist.", added,
                                    added == 1 ? "" : "s");
      if (unknown + badchars > 0 && off < sizeof(buf)) {
         off += (size_t)snprintf(buf + off, sizeof(buf) - off, " %d not recognized.",
                                 unknown + badchars);
      }
      if (full > 0 && off < sizeof(buf)) {
         snprintf(buf + off, sizeof(buf) - off, " %d skipped (list full).", full);
      }
      return strdup(buf);
   }
   if (strcmp(action, "watch_list") == 0) {
      char list[SCHWAB_WATCHLIST_MAX][SCHWAB_SYMBOL_MAX];
      int n = 0;
      schwab_watchlist_list(user_id, list, SCHWAB_WATCHLIST_MAX, &n);
      if (n == 0) {
         return strdup("Your stocks watchlist is empty.");
      }
      char buf[512];
      size_t off = (size_t)snprintf(buf, sizeof(buf), "Watchlist (%d): ", n);
      for (int i = 0; i < n && off < sizeof(buf); i++) {
         off += (size_t)snprintf(buf + off, sizeof(buf) - off, "%s%s", i ? ", " : "", list[i]);
      }
      return strdup(buf);
   }

   return strdup("Unknown stocks action. Use 'quote', 'portfolio', 'accounts', 'history', "
                 "'fundamentals', 'transactions', 'watch_add', 'watch_remove', or 'watch_list'.");
}

/* ========== Public API ========== */

int schwab_tool_register(void) {
   return tool_registry_register(&schwab_metadata);
}
