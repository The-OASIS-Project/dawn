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

/**
 * @brief Effective input tokens saved by prompt caching this turn
 *
 * Computes the discount-weighted input-token savings from a turn's cache
 * activity: cache-READ tokens are billed below full input price (a saving),
 * while cache-WRITE tokens can carry a premium (a cost). The result is the net
 * saving expressed in equivalent full-price input tokens, so a consumer can show
 * "saved N of M input tokens (X%)" without knowing dollar prices.
 *
 * The return value can be NEGATIVE on a pure cache-write turn (the write is an
 * up-front cost that only pays back on later reads) — that is honest, not a bug.
 *
 * Ratios are approximate provider list-price figures (as of early 2026); model-
 * level exceptions exist and OpenRouter varies by upstream vendor, so treat the
 * output as an estimate. Local models have no billing → always 0.
 *
 * @param type LLM_LOCAL (always 0) or LLM_CLOUD
 * @param provider Cloud provider (selects the discount); ignored for LLM_LOCAL
 * @param cached_tokens Cache-read prompt tokens this turn
 * @param cache_write_tokens Cache-write prompt tokens this turn
 * @return Net effective input tokens saved (may be negative)
 */
int llm_cache_saved_input_tokens(llm_type_t type,
                                 cloud_provider_t provider,
                                 int cached_tokens,
                                 int cache_write_tokens);

#ifdef __cplusplus
}
#endif

#endif /* LLM_PRICING_H */
