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
 * LLM cache-pricing economics — the single source of truth for how much prompt
 * caching saves per provider. Deliberately expressed as discount RATIOS (which
 * are far more stable than absolute per-token prices), not a dollar table.
 */

#ifndef LLM_PRICING_H
#define LLM_PRICING_H

#include "llm/llm_interface.h" /* llm_type_t, cloud_provider_t */

#ifdef __cplusplus
extern "C" {
#endif

struct toml_table_t;

/** A model's prompt-cache prices, as multiples of its base input price. */
typedef struct {
   float read;     /**< A cache-read token (e.g. 0.10) */
   float write_5m; /**< A cache-write token, 5-minute TTL (1.0 = no write charge) */
   float write_1h; /**< A cache-write token, 1-hour TTL */
} llm_cache_pricing_t;

/**
 * @brief Load the [cache_pricing.<provider>] tables from models.toml
 *
 * Called once at startup with the parsed file (llm_context owns the parse); a
 * missing table leaves that provider on its defaults.  Read-only afterwards.
 */
void llm_pricing_load_registry(struct toml_table_t *root);

/** Free what llm_pricing_load_registry() loaded. */
void llm_pricing_free_registry(void);

/**
 * @brief A model's cache prices: its longest-prefix models.toml entry, else its
 * provider's default
 *
 * An OpenRouter model ("anthropic/claude-sonnet-5") is priced by its vendor's
 * table.  Local models cost nothing: all multipliers 1.0 and nothing saved.
 */
llm_cache_pricing_t llm_cache_pricing(llm_type_t type,
                                      cloud_provider_t provider,
                                      const char *model);

/**
 * @brief Effective input tokens saved by prompt caching on one call
 *
 * The net saving in full-price input tokens: cache reads are billed below the
 * input price (a saving), cache writes can carry a premium (a cost), per the
 * model's llm_cache_pricing().  Negative on a write-heavy call (the write pays
 * back on later reads).  Local models have no billing: always 0.  An estimate:
 * list prices, 5-minute writes.
 *
 * @param type LLM_LOCAL (always 0) or LLM_CLOUD
 * @param provider Cloud provider; ignored for LLM_LOCAL
 * @param model The model as sent (NULL/"" = the provider's default prices)
 * @param cached_tokens Cache-read prompt tokens
 * @param cache_write_tokens Cache-write prompt tokens
 * @return Net effective input tokens saved (may be negative)
 */
int llm_cache_saved_input_tokens(llm_type_t type,
                                 cloud_provider_t provider,
                                 const char *model,
                                 int cached_tokens,
                                 int cache_write_tokens);

#ifdef __cplusplus
}
#endif

#endif /* LLM_PRICING_H */
