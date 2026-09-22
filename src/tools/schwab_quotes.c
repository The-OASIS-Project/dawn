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
 * Charles Schwab /quotes response parser — see schwab_quotes.h.
 */

#include "tools/schwab_quotes.h"

#include <json-c/json.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dawn_error.h"

/* Read a string member, "" if absent. */
static const char *jqs(struct json_object *o, const char *k) {
   struct json_object *v = NULL;
   return (o && json_object_object_get_ex(o, k, &v)) ? json_object_get_string(v) : "";
}

/* Read a double member, 0.0 if absent. */
static double jqd(struct json_object *o, const char *k) {
   struct json_object *v = NULL;
   return (o && json_object_object_get_ex(o, k, &v)) ? json_object_get_double(v) : 0.0;
}

int schwab_quotes_parse(struct json_object *root, schwab_quote_t *out, int max, int *n_out) {
   if (n_out) {
      *n_out = 0;
   }
   if (!out || max <= 0 || !root || !json_object_is_type(root, json_type_object)) {
      return FAILURE;
   }

   int n = 0;
   json_object_object_foreach(root, key, val) {
      if (n >= max) {
         break;
      }
      /* The response mixes symbol entries with an "errors" object (invalidSymbols
       * etc.); skip that and any non-object member. */
      if (strcmp(key, "errors") == 0 || !json_object_is_type(val, json_type_object)) {
         continue;
      }
      schwab_quote_t *q = &out[n];
      memset(q, 0, sizeof(*q));
      snprintf(q->symbol, sizeof(q->symbol), "%s", key);
      q->quotable = true; /* Schwab returned this symbol */
      q->asset_type = schwab_asset_type_from_str(jqs(val, "assetMainType"));
      struct json_object *ref = NULL;
      if (json_object_object_get_ex(val, "reference", &ref)) {
         snprintf(q->description, sizeof(q->description), "%s", jqs(ref, "description"));
      }
      struct json_object *quote = NULL;
      if (json_object_object_get_ex(val, "quote", &quote)) {
         q->last = jqd(quote, "lastPrice");
         q->net_change = jqd(quote, "netChange");
         q->net_change_pct = jqd(quote, "netPercentChange");
      }
      /* Extended-hours overlay — present only when the caller requested the
       * regular+extended fields (during an active pre/post window). The move is
       * measured against the regular-session close (regular.regularMarketLastPrice),
       * falling back to the regular quote last when the regular block is absent. */
      struct json_object *reg = NULL, *ext = NULL;
      double reg_close = 0.0;
      if (json_object_object_get_ex(val, "regular", &reg)) {
         reg_close = jqd(reg, "regularMarketLastPrice");
      }
      if (reg_close <= 0.0) {
         reg_close = q->last;
      }
      if (json_object_object_get_ex(val, "extended", &ext)) {
         double el = jqd(ext, "lastPrice");
         if (el > 0.0) {
            q->has_ext = true;
            q->ext_last = el;
            if (reg_close > 0.0) {
               q->ext_change = el - reg_close;
               q->ext_change_pct = q->ext_change / reg_close * 100.0;
            }
         }
      }
      n++;
   }

   if (n_out) {
      *n_out = n;
   }
   return SUCCESS;
}

int schwab_quotes_parse_invalid(struct json_object *root,
                                char (*out)[SCHWAB_SYMBOL_MAX],
                                int max,
                                int *n_out) {
   if (n_out) {
      *n_out = 0;
   }
   if (!out || max <= 0 || !root || !json_object_is_type(root, json_type_object)) {
      return FAILURE;
   }
   struct json_object *errors = NULL, *invalid = NULL;
   if (json_object_object_get_ex(root, "errors", &errors) &&
       json_object_object_get_ex(errors, "invalidSymbols", &invalid) &&
       json_object_is_type(invalid, json_type_array)) {
      size_t ni = json_object_array_length(invalid);
      int n = 0;
      for (size_t i = 0; i < ni && n < max; i++) {
         const char *s = json_object_get_string(json_object_array_get_idx(invalid, i));
         if (s && s[0]) {
            snprintf(out[n], SCHWAB_SYMBOL_MAX, "%s", s);
            n++;
         }
      }
      if (n_out) {
         *n_out = n;
      }
   }
   return SUCCESS;
}

/* Emit the shared per-row ext-hours object {price, change, change_pct} into @p o.
 * `change`/`change_pct` are always PER-SHARE (ext_last − regular close) on BOTH the
 * watch and portfolio payloads — identical units so the consumer feature-detects one
 * `ext` shape. NOTE for the portfolio surface: the sibling `day_change` on a position
 * row is a position TOTAL, while `ext.change` here is per-share (aligns with `price`);
 * a consumer wanting a position-level after-hours dollar figure multiplies by `qty`. */
void schwab_quotes_add_ext_obj(struct json_object *o,
                               double ext_last,
                               double ext_change,
                               double ext_change_pct) {
   struct json_object *e = json_object_new_object();
   json_object_object_add(e, "price", json_object_new_double(ext_last));
   json_object_object_add(e, "change", json_object_new_double(ext_change));
   json_object_object_add(e, "change_pct", json_object_new_double(ext_change_pct));
   json_object_object_add(o, "ext", e);
}

struct json_object *schwab_quotes_watch_payload_jobj(const schwab_quote_t *q,
                                                     int n,
                                                     const char *status,
                                                     const char *market,
                                                     int64_t as_of) {
   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "status", json_object_new_string(status ? status : "error"));
   json_object_object_add(payload, "market", json_object_new_string(market ? market : "closed"));
   json_object_object_add(payload, "as_of", json_object_new_int64(as_of));

   /* Emit the ext overlay only while the session is actually pre/post. The cache can
    * hold has_ext quotes fetched in the last window and re-serve them after the market
    * closes (no fetch happens when closed), so gating on has_ext alone would leak a
    * stale ext price into a "closed" frame — build-time market gating keeps the wire
    * matching the contract regardless of cache age. */
   bool ext_session = market && (strcmp(market, "pre") == 0 || strcmp(market, "post") == 0);

   struct json_object *arr = json_object_new_array();
   for (int i = 0; q && i < n; i++) {
      struct json_object *o = json_object_new_object();
      json_object_object_add(o, "symbol", json_object_new_string(q[i].symbol));
      if (!q[i].quotable) {
         /* A stored ticker Schwab could not quote (bad/unknown symbol) → a minimal
          * flagged row so the client dims it and keeps the remove affordance, rather
          * than the symbol silently vanishing. Quotable rows omit the flag. */
         json_object_object_add(o, "quotable", json_object_new_boolean(false));
         json_object_array_add(arr, o);
         continue;
      }
      if (q[i].description[0]) {
         json_object_object_add(o, "description", json_object_new_string(q[i].description));
      }
      json_object_object_add(o, "asset_type",
                             json_object_new_string(schwab_asset_type_str(q[i].asset_type)));
      json_object_object_add(o, "price", json_object_new_double(q[i].last));
      json_object_object_add(o, "day_change", json_object_new_double(q[i].net_change));
      json_object_object_add(o, "day_change_pct", json_object_new_double(q[i].net_change_pct));
      if (q[i].has_ext && ext_session) {
         schwab_quotes_add_ext_obj(o, q[i].ext_last, q[i].ext_change, q[i].ext_change_pct);
      }
      json_object_array_add(arr, o);
   }
   json_object_object_add(payload, "symbols", arr);
   return payload;
}
