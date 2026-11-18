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
 * LLM pricing from models.toml [pricing.<provider>]: base prices, cache-price
 * multiples, and the derived per-turn input-token savings. See llm_pricing.h.
 */

#include "llm/llm_pricing.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_model_family.h"
#include "logging.h"
#include "toml.h"

/* Provider defaults, for a model models.toml doesn't list (list prices,
 * September 2026): Anthropic reads 0.1x and writes 1.25x (5 min) / 2x (1 h);
 * OpenAI GPT-5.x and Gemini read 0.1x with no write charge. */
static const llm_cache_pricing_t DEFAULT_ANTHROPIC = { 0.10f, 1.25f, 2.00f };
static const llm_cache_pricing_t DEFAULT_OPENAI = { 0.10f, 1.00f, 1.00f };
static const llm_cache_pricing_t DEFAULT_GEMINI = { 0.10f, 1.00f, 1.00f };
/* OpenRouter with an unrecognized vendor: a rough middle. */
static const llm_cache_pricing_t DEFAULT_OTHER = { 0.50f, 1.00f, 1.00f };
static const llm_cache_pricing_t NO_BILLING = { 1.00f, 1.00f, 1.00f };

/* A base price ($ per million tokens) past this is a typo, not a price. */
#define PRICE_USD_PER_M_MAX 100000.0

typedef struct {
   char *prefix;
   llm_cache_pricing_t price;
} cache_entry_t;

typedef struct {
   char *prefix;
   double input;  /* $ per million input tokens */
   double output; /* $ per million output tokens */
} rate_entry_t;

/* One provider's two lists, each longest prefix first.  A row can carry base
 * prices, cache multiples or both, and each list is matched on its own: a row
 * with only cache multiples ("claude-sonnet-5-5") takes its base price from the
 * longest shorter row that has one ("claude-sonnet-5"), and the other way round
 * ("gpt-4o-mini" reads at the "gpt-4o" row's multiples). */
typedef struct {
   cache_entry_t *cache;
   int n_cache;
   rate_entry_t *rates;
   int n_rates;
} provider_prices_t;

enum {
   PRICES_ANTHROPIC,
   PRICES_OPENAI,
   PRICES_GEMINI,
   PRICES_COUNT
};
static const char *const PROVIDER_KEYS[PRICES_COUNT] = { "anthropic", "openai", "gemini" };
static const llm_cache_pricing_t *const PROVIDER_DEFAULTS[PRICES_COUNT] = { &DEFAULT_ANTHROPIC,
                                                                            &DEFAULT_OPENAI,
                                                                            &DEFAULT_GEMINI };

/* What route() returns besides a provider table. */
#define ROUTE_OTHER_VENDOR (-1) /* another OpenRouter vendor */
#define ROUTE_NO_BILLING (-2)   /* a local model */

/* Loaded once at startup, read-only afterwards. */
static provider_prices_t s_prices[PRICES_COUNT];

static int cache_cmp_desc(const void *a, const void *b) {
   const size_t la = strlen(((const cache_entry_t *)a)->prefix);
   const size_t lb = strlen(((const cache_entry_t *)b)->prefix);
   return la < lb ? 1 : la > lb ? -1 : 0;
}

static int rate_cmp_desc(const void *a, const void *b) {
   const size_t la = strlen(((const rate_entry_t *)a)->prefix);
   const size_t lb = strlen(((const rate_entry_t *)b)->prefix);
   return la < lb ? 1 : la > lb ? -1 : 0;
}

/* A number from an entry's inline table, integer or float. */
static bool number_in(toml_table_t *entry, const char *key, double *out) {
   toml_datum_t d = toml_double_in(entry, key);
   if (d.ok) {
      *out = d.u.d;
      return true;
   }
   toml_datum_t i = toml_int_in(entry, key);
   if (i.ok) {
      *out = (double)i.u.i;
      return true;
   }
   return false;
}

/* A multiplier; @p fallback when absent or out of range (0..10). */
static float price_field(toml_table_t *entry, const char *key, float fallback) {
   double v;
   if (!number_in(entry, key, &v)) {
      return fallback;
   }
   return (v >= 0.0 && v <= 10.0) ? (float)v : fallback;
}

static bool has_key(toml_table_t *entry, const char *key) {
   return toml_raw_in(entry, key) != NULL;
}

static void free_cache(provider_prices_t *p) {
   for (int i = 0; i < p->n_cache; i++) {
      free(p->cache[i].prefix);
   }
   free(p->cache);
   p->cache = NULL;
   p->n_cache = 0;
}

static void free_rates(provider_prices_t *p) {
   for (int i = 0; i < p->n_rates; i++) {
      free(p->rates[i].prefix);
   }
   free(p->rates);
   p->rates = NULL;
   p->n_rates = 0;
}

/* An entry's base prices: both input and output, each 0..PRICE_USD_PER_M_MAX.
 * False (logged) when it lists one without the other, or a bad value. */
static bool parse_rates(const char *table,
                        const char *provider,
                        const char *prefix,
                        toml_table_t *entry,
                        double *in,
                        double *out) {
   const bool has_in = number_in(entry, "input", in);
   const bool has_out = number_in(entry, "output", out);
   if (!has_in || !has_out) {
      OLOG_WARNING("models.toml [%s.%s] \"%s\": a base price needs both input and output; "
                   "ignoring it",
                   table, provider, prefix);
      return false;
   }
   if (*in < 0.0 || *in > PRICE_USD_PER_M_MAX || *out < 0.0 || *out > PRICE_USD_PER_M_MAX) {
      OLOG_WARNING("models.toml [%s.%s] \"%s\": ignoring base price %g / %g", table, provider,
                   prefix, *in, *out);
      return false;
   }
   return true;
}

/* One provider's table: "prefix" = { input, output, read, write_5m, write_1h },
 * either group or both. */
static void load_provider(toml_table_t *tab, const char *table, int idx, provider_prices_t *p) {
   const int n = tab ? toml_table_ntab(tab) : 0;
   if (n <= 0) {
      return;
   }
   p->cache = calloc((size_t)n, sizeof(*p->cache));
   p->rates = calloc((size_t)n, sizeof(*p->rates));
   if (!p->cache || !p->rates) {
      free(p->cache);
      free(p->rates);
      p->cache = NULL;
      p->rates = NULL;
      return;
   }
   const llm_cache_pricing_t *defaults = PROVIDER_DEFAULTS[idx];
   const char *key = NULL;
   for (int i = 0; (key = toml_key_in(tab, i)) != NULL; i++) {
      toml_table_t *entry = toml_table_in(tab, key);
      if (!entry || !*key) {
         continue;
      }
      if (has_key(entry, "read") || has_key(entry, "write_5m") || has_key(entry, "write_1h")) {
         cache_entry_t *c = &p->cache[p->n_cache];
         c->prefix = strdup(key);
         if (c->prefix) {
            c->price.read = price_field(entry, "read", defaults->read);
            c->price.write_5m = price_field(entry, "write_5m", defaults->write_5m);
            c->price.write_1h = price_field(entry, "write_1h", defaults->write_1h);
            p->n_cache++;
         }
      }
      if (has_key(entry, "input") || has_key(entry, "output")) {
         double in = 0.0, out = 0.0;
         if (parse_rates(table, PROVIDER_KEYS[idx], key, entry, &in, &out)) {
            rate_entry_t *r = &p->rates[p->n_rates];
            r->prefix = strdup(key);
            if (r->prefix) {
               r->input = in;
               r->output = out;
               p->n_rates++;
            }
         }
      }
   }
   qsort(p->cache, (size_t)p->n_cache, sizeof(*p->cache), cache_cmp_desc);
   qsort(p->rates, (size_t)p->n_rates, sizeof(*p->rates), rate_cmp_desc);
}

void llm_pricing_load_registry(struct toml_table_t *root) {
   llm_pricing_free_registry();
   toml_table_t *pricing = root ? toml_table_in(root, "pricing") : NULL;
   for (int i = 0; pricing && i < PRICES_COUNT; i++) {
      load_provider(toml_table_in(pricing, PROVIDER_KEYS[i]), "pricing", i, &s_prices[i]);
   }
}

void llm_pricing_free_registry(void) {
   for (int i = 0; i < PRICES_COUNT; i++) {
      free_cache(&s_prices[i]);
      free_rates(&s_prices[i]);
   }
}

/* The provider table @p model is priced from, with its id in that table's form
 * (an OpenRouter "vendor/model" is priced as its vendor prices it), or a
 * ROUTE_* value. */
static int route(llm_type_t type,
                 cloud_provider_t provider,
                 const char *model,
                 char *id,
                 size_t id_len) {
   if (type != LLM_CLOUD) {
      return ROUTE_NO_BILLING;
   }
   switch (llm_model_route(type, provider, model, id, id_len)) {
      case LLM_FAMILY_ANTHROPIC:
         return PRICES_ANTHROPIC;
      case LLM_FAMILY_OPENAI:
         return PRICES_OPENAI;
      case LLM_FAMILY_GEMINI:
         return PRICES_GEMINI;
      default:
         return ROUTE_OTHER_VENDOR;
   }
}

static bool has_prefix(const char *id, const char *prefix) {
   return strncmp(id, prefix, strlen(prefix)) == 0;
}

llm_cache_pricing_t llm_cache_pricing(llm_type_t type,
                                      cloud_provider_t provider,
                                      const char *model) {
   char id[96];
   const int idx = route(type, provider, model, id, sizeof(id));
   if (idx == ROUTE_NO_BILLING) {
      return NO_BILLING;
   }
   if (idx == ROUTE_OTHER_VENDOR) {
      return DEFAULT_OTHER;
   }
   const provider_prices_t *p = &s_prices[idx];
   for (int i = 0; model && i < p->n_cache; i++) {
      if (has_prefix(id, p->cache[i].prefix)) {
         return p->cache[i].price;
      }
   }
   return *PROVIDER_DEFAULTS[idx];
}

bool llm_pricing_rate(llm_type_t type,
                      cloud_provider_t provider,
                      const char *model,
                      double *input_usd_per_m,
                      double *output_usd_per_m) {
   if (!model || !*model) {
      return false;
   }
   char id[96];
   const int idx = route(type, provider, model, id, sizeof(id));
   if (idx < 0) {
      return false;
   }
   const provider_prices_t *p = &s_prices[idx];
   for (int i = 0; i < p->n_rates; i++) {
      if (has_prefix(id, p->rates[i].prefix)) {
         if (input_usd_per_m) {
            *input_usd_per_m = p->rates[i].input;
         }
         if (output_usd_per_m) {
            *output_usd_per_m = p->rates[i].output;
         }
         return true;
      }
   }
   return false;
}

int llm_cache_saved_input_tokens(llm_type_t type,
                                 cloud_provider_t provider,
                                 const char *model,
                                 int cached_tokens,
                                 int cache_write_tokens) {
   /* Local inference has no billing, so caching saves nothing measurable here. */
   if (type != LLM_CLOUD) {
      return 0;
   }
   if (cached_tokens < 0) {
      cached_tokens = 0;
   }
   if (cache_write_tokens < 0) {
      cache_write_tokens = 0;
   }
   /* DAWN writes with the default 5-minute TTL. */
   const llm_cache_pricing_t p = llm_cache_pricing(type, provider, model);
   float saved = (float)cached_tokens * (1.0f - p.read) -
                 (float)cache_write_tokens * (p.write_5m - 1.0f);
   /* Round to the nearest whole input token; may be negative on a write-only turn. */
   return (int)lroundf(saved);
}
