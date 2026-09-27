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
 * LLM cache-pricing economics — provider cache-discount ratios and the derived
 * per-turn input-token savings. See llm_pricing.h for the contract.
 */

#include "llm/llm_pricing.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"
#include "tools/toml.h"

/* Provider defaults, for a model models.toml doesn't list (list prices,
 * September 2026): Anthropic reads 0.1x and writes 1.25x (5 min) / 2x (1 h);
 * OpenAI GPT-5.x and Gemini read 0.1x with no write charge. */
static const llm_cache_pricing_t DEFAULT_ANTHROPIC = { 0.10f, 1.25f, 2.00f };
static const llm_cache_pricing_t DEFAULT_OPENAI = { 0.10f, 1.00f, 1.00f };
static const llm_cache_pricing_t DEFAULT_GEMINI = { 0.10f, 1.00f, 1.00f };
/* OpenRouter with an unrecognized vendor: a rough middle. */
static const llm_cache_pricing_t DEFAULT_OTHER = { 0.50f, 1.00f, 1.00f };
static const llm_cache_pricing_t NO_BILLING = { 1.00f, 1.00f, 1.00f };

typedef struct {
   char *prefix; /* NULL-terminated array */
   llm_cache_pricing_t price;
} price_entry_t;

/* Longest prefix first; loaded once at startup, read-only afterwards. */
static price_entry_t *s_anthropic_prices;
static price_entry_t *s_openai_prices;
static price_entry_t *s_gemini_prices;

static int price_entry_cmp_desc(const void *a, const void *b) {
   const size_t la = strlen(((const price_entry_t *)a)->prefix);
   const size_t lb = strlen(((const price_entry_t *)b)->prefix);
   return la < lb ? 1 : la > lb ? -1 : 0;
}

/* A multiplier from an entry's inline table; @p fallback when absent or out
 * of range (0..10). */
static float price_field(toml_table_t *entry, const char *key, float fallback) {
   toml_datum_t d = toml_double_in(entry, key);
   if (!d.ok) {
      toml_datum_t i = toml_int_in(entry, key);
      if (!i.ok) {
         return fallback;
      }
      d.u.d = (double)i.u.i;
   }
   return (d.u.d >= 0.0 && d.u.d <= 10.0) ? (float)d.u.d : fallback;
}

/* [cache_pricing.<provider>]: "prefix" = { read = .., write_5m = .., write_1h = .. } */
static price_entry_t *load_prices(toml_table_t *pricing,
                                  const char *provider,
                                  const llm_cache_pricing_t *defaults) {
   toml_table_t *tab = pricing ? toml_table_in(pricing, provider) : NULL;
   const int n = tab ? toml_table_ntab(tab) : 0;
   if (n <= 0) {
      return NULL;
   }
   price_entry_t *arr = calloc((size_t)n + 1, sizeof(*arr));
   if (!arr) {
      return NULL;
   }
   int count = 0;
   const char *key = NULL;
   for (int i = 0; (key = toml_key_in(tab, i)) != NULL && count < n; i++) {
      toml_table_t *entry = toml_table_in(tab, key);
      if (!entry || !*key) {
         continue;
      }
      arr[count].prefix = strdup(key);
      if (!arr[count].prefix) {
         continue;
      }
      arr[count].price.read = price_field(entry, "read", defaults->read);
      arr[count].price.write_5m = price_field(entry, "write_5m", defaults->write_5m);
      arr[count].price.write_1h = price_field(entry, "write_1h", defaults->write_1h);
      count++;
   }
   if (count == 0) {
      free(arr);
      return NULL;
   }
   qsort(arr, (size_t)count, sizeof(*arr), price_entry_cmp_desc);
   return arr;
}

static void free_prices(price_entry_t **arr) {
   if (!arr || !*arr) {
      return;
   }
   for (int i = 0; (*arr)[i].prefix; i++) {
      free((*arr)[i].prefix);
   }
   free(*arr);
   *arr = NULL;
}

void llm_pricing_load_registry(struct toml_table_t *root) {
   llm_pricing_free_registry();
   toml_table_t *pricing = root ? toml_table_in(root, "cache_pricing") : NULL;
   if (!pricing) {
      return;
   }
   s_anthropic_prices = load_prices(pricing, "anthropic", &DEFAULT_ANTHROPIC);
   s_openai_prices = load_prices(pricing, "openai", &DEFAULT_OPENAI);
   s_gemini_prices = load_prices(pricing, "gemini", &DEFAULT_GEMINI);
}

void llm_pricing_free_registry(void) {
   free_prices(&s_anthropic_prices);
   free_prices(&s_openai_prices);
   free_prices(&s_gemini_prices);
}

static llm_cache_pricing_t lookup(const price_entry_t *arr,
                                  const char *model,
                                  const llm_cache_pricing_t *defaults) {
   for (int i = 0; arr && model && arr[i].prefix; i++) {
      if (strncmp(model, arr[i].prefix, strlen(arr[i].prefix)) == 0) {
         return arr[i].price;
      }
   }
   return *defaults;
}

/* Anthropic's own ids spell versions with dashes (claude-opus-5-5); OpenRouter
 * with dots (anthropic/claude-opus-5.5).  Look both up the Anthropic way. */
static llm_cache_pricing_t lookup_anthropic(const char *model) {
   char id[96];
   size_t n = 0;
   for (; model && model[n] && n < sizeof(id) - 1; n++) {
      const bool version_dot = model[n] == '.' && n > 0 && model[n - 1] >= '0' &&
                               model[n - 1] <= '9' && model[n + 1] >= '0' && model[n + 1] <= '9';
      id[n] = version_dot ? '-' : model[n];
   }
   id[n] = '\0';
   return lookup(s_anthropic_prices, id, &DEFAULT_ANTHROPIC);
}

llm_cache_pricing_t llm_cache_pricing(llm_type_t type,
                                      cloud_provider_t provider,
                                      const char *model) {
   if (type != LLM_CLOUD) {
      return NO_BILLING;
   }
   switch (provider) {
      case CLOUD_PROVIDER_CLAUDE:
         return lookup_anthropic(model);
      case CLOUD_PROVIDER_OPENAI:
         return lookup(s_openai_prices, model, &DEFAULT_OPENAI);
      case CLOUD_PROVIDER_GEMINI:
         return lookup(s_gemini_prices, model, &DEFAULT_GEMINI);
      case CLOUD_PROVIDER_OPENROUTER: {
         /* "vendor/model": priced as the vendor prices it. */
         const char *slash = model ? strchr(model, '/') : NULL;
         if (slash) {
            const size_t vlen = (size_t)(slash - model);
            if (vlen == 9 && strncmp(model, "anthropic", 9) == 0) {
               return lookup_anthropic(slash + 1);
            }
            if (vlen == 6 && strncmp(model, "openai", 6) == 0) {
               return lookup(s_openai_prices, slash + 1, &DEFAULT_OPENAI);
            }
            if (vlen == 6 && strncmp(model, "google", 6) == 0) {
               return lookup(s_gemini_prices, slash + 1, &DEFAULT_GEMINI);
            }
         }
         return DEFAULT_OTHER;
      }
      default:
         return DEFAULT_OTHER;
   }
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
