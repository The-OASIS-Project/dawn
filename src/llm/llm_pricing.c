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

/* Fraction of a cache-READ input token's full price that is SAVED (i.e. cached
 * tokens are billed at (1 - savings) of base input). Approximate provider list
 * prices, early 2026; model-level exceptions exist. */
#define CACHE_READ_SAVINGS_ANTHROPIC 0.90f  /* cache read ~= 0.10x input */
#define CACHE_READ_SAVINGS_OPENAI 0.50f     /* cached input ~= 0.50x */
#define CACHE_READ_SAVINGS_GEMINI 0.75f     /* implicit caching ~= 0.25x (2.5 tier) */
#define CACHE_READ_SAVINGS_OPENROUTER 0.50f /* varies by upstream vendor; rough default */

/* Extra fraction of full input price paid per cache-WRITE token (a premium).
 * Anthropic charges 1.25x for a 5-minute-TTL write; OpenAI/Gemini bill automatic
 * caching with no separate write charge. */
#define CACHE_WRITE_PREMIUM_ANTHROPIC 0.25f
#define CACHE_WRITE_PREMIUM_OPENAI 0.0f
#define CACHE_WRITE_PREMIUM_GEMINI 0.0f
#define CACHE_WRITE_PREMIUM_OPENROUTER 0.0f

static float cache_read_savings_fraction(cloud_provider_t provider) {
   switch (provider) {
      case CLOUD_PROVIDER_CLAUDE:
         return CACHE_READ_SAVINGS_ANTHROPIC;
      case CLOUD_PROVIDER_OPENAI:
         return CACHE_READ_SAVINGS_OPENAI;
      case CLOUD_PROVIDER_GEMINI:
         return CACHE_READ_SAVINGS_GEMINI;
      case CLOUD_PROVIDER_OPENROUTER:
         return CACHE_READ_SAVINGS_OPENROUTER;
      default:
         return 0.0f;
   }
}

static float cache_write_premium_fraction(cloud_provider_t provider) {
   switch (provider) {
      case CLOUD_PROVIDER_CLAUDE:
         return CACHE_WRITE_PREMIUM_ANTHROPIC;
      case CLOUD_PROVIDER_OPENAI:
         return CACHE_WRITE_PREMIUM_OPENAI;
      case CLOUD_PROVIDER_GEMINI:
         return CACHE_WRITE_PREMIUM_GEMINI;
      case CLOUD_PROVIDER_OPENROUTER:
         return CACHE_WRITE_PREMIUM_OPENROUTER;
      default:
         return 0.0f;
   }
}

int llm_cache_saved_input_tokens(llm_type_t type,
                                 cloud_provider_t provider,
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

   float saved = (float)cached_tokens * cache_read_savings_fraction(provider) -
                 (float)cache_write_tokens * cache_write_premium_fraction(provider);
   /* Round to the nearest whole input token; may be negative on a write-only turn. */
   return (int)lroundf(saved);
}
