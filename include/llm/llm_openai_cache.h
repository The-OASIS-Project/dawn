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
 * A chat-completions request's leading system messages, merged into one.  Split
 * out of llm_openai_chat_completions.c so the (json-c-only) logic is
 * unit-testable without the provider's curl/config/streaming deps.
 */

#ifndef LLM_OPENAI_CACHE_H
#define LLM_OPENAI_CACHE_H

#include <json-c/json.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Collapse the contiguous run of leading plain-string system messages
 *        into a single system message.
 *
 * DAWN emits the system prompt as TWO consecutive system messages (stable prefix
 * + volatile tail) purely as an Anthropic explicit-cache boundary marker.  The
 * real OpenAI API tolerates multiple system messages, but a strict local Jinja
 * chat template (Qwen 3.5/3.6) hard-raises "System message must be at the
 * beginning" on the second one → HTTP 500.  For providers that do NOT use the
 * Anthropic breakpoint (native OpenAI implicit caching, llama.cpp/Ollama KV
 * prefix reuse) the split is inert, so merging is safe and fixes the rejection.
 *
 * Merges only the leading run whose messages carry plain-string content; a
 * non-string (e.g. already cache-wrapped) system message ends the run.  No-op if
 * fewer than two such messages lead the array.  Bodies are joined with a blank
 * line ("\n\n").
 *
 * Copy-on-write: @p root's "messages" array may ALIAS the session's canonical
 * conversation_history, so the merged run is written into a request-private clone
 * of the messages array; the session's history object is never mutated (same
 * contract.
 *
 * @param root Request body (its "messages" array may be swapped for a private clone).
 */
void llm_openai_merge_leading_system_messages(struct json_object *root);

#ifdef __cplusplus
}
#endif

#endif /* LLM_OPENAI_CACHE_H */
