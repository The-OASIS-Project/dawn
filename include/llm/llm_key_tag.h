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
 * A tag that tells API keys apart without revealing them: part of the carrier
 * a vendor's reasoning is replayed to (llm_turn_blocks_carrier).
 */

#ifndef LLM_KEY_TAG_H
#define LLM_KEY_TAG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Room for a key tag: 16 hex digits and the terminator. */
#define LLM_KEY_TAG_MAX 17

/**
 * @brief The tag of an API key
 *
 * A 64-bit keyed digest (crypto_store_keyed_digest) in hex.  Stable across
 * restarts on this install, so stored reasoning replays to the key that
 * issued it; meaningless elsewhere, so a copied database replays nothing, and
 * a stored tag gives no way to test guesses at a key.  If the key store can't
 * be used, a per-process key stands in: keys stay apart, and stored reasoning
 * is not replayed after a restart.
 *
 * @param api_key The key (NULL or "" for none)
 * @param out     At least LLM_KEY_TAG_MAX bytes
 */
void llm_key_tag(const char *api_key, char *out, size_t out_len);

/**
 * @brief The carrier of a request: its endpoint and its key's tag
 *
 * llm_turn_blocks_carrier(base_url, llm_key_tag(api_key)).  Whose reasoning a
 * request may send back, and whose the reasoning it receives is.
 *
 * @param out At least LLM_CARRIER_MAX bytes
 */
void llm_request_carrier(const char *base_url, const char *api_key, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* LLM_KEY_TAG_H */
